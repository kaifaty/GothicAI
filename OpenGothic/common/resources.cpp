#include "resources.h"
#include "loadprobe.h"
#include "basestore.h"
#include "commandline.h"
#include "musicnative.h"
#include "videodata.h"
#include "frameprofile.h"

#include <Tempest/MemReader>
#include <Tempest/Pixmap>
#include <Tempest/Device>
#include <Tempest/Application>
#include <Tempest/Sound>
#include <Tempest/SoundEffect>
#include <Tempest/TextCodec>
#include <Tempest/Log>
#include <Tempest/Color>

#include "asset/material.h"
#include "asset/worldtypes.h"
#include <miniz.h>

#include <cctype>
#include <cassert>
#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <thread>
#include <cstdio>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
#include <rapidjson/document.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include "graphics/mesh/submesh/pfxemittermesh.h"
#include "graphics/mesh/submesh/packedmesh.h"
#include "graphics/mesh/skeleton.h"
#include "graphics/mesh/protomesh.h"
#include "graphics/mesh/animation.h"
#include "graphics/mesh/attachbinder.h"
#include "graphics/mesh/bowdraw.h"
#include "graphics/material.h"
#include "dmusic/directmusic.h"
#include "utils/fileext.h"
#include "utils/gthfont.h"

#include "gothic.h"
#include "contentcensus.h"
#include "gltfmesh.h"
#include "basemesh.h"
#include "modeldata.h"
#include "fontdata.h"
#include "worldbase.h"
#include "world/chunkresidency.h"
#include "utils/string_frm.h"

#include <dmusic.h>
#include <optional>
#include <unordered_map>

using namespace Tempest;

Resources* Resources::inst=nullptr;

struct Resources::VideoBase {
  std::shared_ptr<std::atomic_bool> cancelled=std::make_shared<std::atomic_bool>(false);
  std::mutex sync;
  BaseStore::Selection selection;
  std::map<std::string,std::filesystem::path> files;
  bool ready=false;
  };

namespace {
thread_local bool preparingVideo = false;

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

std::string censusSourcePath(const std::filesystem::path& source) {
  auto relative = source;
  if(relative.is_absolute())
    relative = relative.lexically_relative(std::filesystem::path(CommandLine::inst().rootPath()));
  relative = relative.lexically_normal();
  if(relative.empty() || relative.is_absolute())
    return {};
  for(const auto& part:relative)
    if(part=="..")
      return {};
  return relative.generic_string();
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

Resources::Resources() {
  inst=this;
  configureTextures();
  initCensus();
  indexMeshOverrides();
  // Sound buffers remain CPU data: native sound selection consumes RNG and
  // dialogue uses their durations even when playback is disabled.
  fBuff.reserve(8*1024*1024);
  }

Resources::Resources(Tempest::Device &device)
  : dev(&device), sound(std::make_unique<Tempest::SoundDevice>()) {
  inst=this;
  configureTextures();
  initCensus();

  static const uint16_t index[] = {
      0, 1, 2, 0, 2, 3,
      4, 6, 5, 4, 7, 6,
      1, 5, 2, 2, 5, 6,
      4, 0, 7, 7, 0, 3,
      3, 2, 7, 7, 2, 6,
      4, 5, 0, 0, 5, 1
    };
  cube = device.ibo(index, sizeof(index)/sizeof(index[0]));

  const std::vector<std::u16string> musicPaths = {
    u"_work/Data/Music/newworld/",
    u"_work/Data/Music/AddonWorld/",
    u"_work/Data/Music/dungeon/",
    u"_work/Data/Music/menu_men/",
    u"_work/Data/Music/orchestra/",
    u"_work/Data/Music/"
    };
  Dx8::DirectMusic::Native dxSource;
  dxSource.segment = [this,musicPaths](const char16_t* name) {
    return MusicData::decodeDx8Segment(readDxMusic(musicPaths,name,"segment"));
    };
  dxSource.style = [this,musicPaths](const char16_t* name) {
    return MusicData::decodeDx8Style(readDxMusic(musicPaths,name,"style"));
    };
  dxSource.dls = [this,musicPaths](const char16_t* name) {
    return MusicData::decodeDx8Dls(readDxMusic(musicPaths,name,"dls"));
    };
  dxMusic.reset(new Dx8::DirectMusic(std::move(dxSource)));

  fBuff .reserve(8*1024*1024);
  indexHdTextures();
  indexMaterialMaps();
  indexMeshOverrides();

  {
  Pixmap pm(1,1,TextureFormat::RGBA8);
  uint8_t* pix = reinterpret_cast<uint8_t*>(pm.data());
  pix[0]=255;
  pix[3]=255;
  fallback = device.texture(pm);
  }

  {
  Pixmap pm(1,1,TextureFormat::RGBA8);
  fbZero = device.texture(pm);
  }

  fbImg = device.image2d(TextureFormat::R32U,1,1);

  // Set up the DirectMusic loader
  DmResult rv = DmLoader_create(&dmLoader, DmLoader_DOWNLOAD);
  if(rv != DmResult_SUCCESS) {
    Log::e("Failed to created DmLoader object. Out of memory?");
    }

  DmLoaderFactory gkSource;
  gkSource.segment = [](void* ctx, const char* name, DmSegment** out) {
    *out = nullptr;
    std::vector<uint8_t> bytes;
    const auto result = static_cast<Resources*>(ctx)->readGkMusic(name,"segment",bytes);
    return result==DmResult_SUCCESS ? MusicNative_segment(bytes.data(),bytes.size(),out) : result;
    };
  gkSource.style = [](void* ctx, const char* name, DmStyle** out) {
    *out = nullptr;
    std::vector<uint8_t> bytes;
    const auto result = static_cast<Resources*>(ctx)->readGkMusic(name,"style",bytes);
    return result==DmResult_SUCCESS ? MusicNative_style(bytes.data(),bytes.size(),out) : result;
    };
  gkSource.dls = [](void* ctx, const char* name, DmDls** out) {
    *out = nullptr;
    std::vector<uint8_t> bytes;
    const auto result = static_cast<Resources*>(ctx)->readGkMusic(name,"dls",bytes);
    return result==DmResult_SUCCESS ? MusicNative_dls(bytes.data(),bytes.size(),out) : result;
    };
  if(dmLoader!=nullptr && DmLoader_setFactory(dmLoader,&gkSource,this)!=DmResult_SUCCESS)
    throw std::runtime_error("unable to configure native music loader");
  }

void Resources::zenkitLog(zenkit::LogLevel lvl, const char* cat, const char* message) {
  if(preparingVideo) return;
  switch (lvl) {
    case zenkit::LogLevel::ERROR:
      Tempest::Log::e("[zenkit] ", message);
      break;
    case zenkit::LogLevel::WARNING:
      Tempest::Log::e("[zenkit] ", message);
      break;
    case zenkit::LogLevel::INFO:
      Tempest::Log::i("[zenkit] ", message);
      break;
    case zenkit::LogLevel::DEBUG:
    case zenkit::LogLevel::TRACE:
      Tempest::Log::d("[zenkit] ", message); // unused
      break;
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

Resources::~Resources() {
  if(textureReload && textureReload->pending.valid())
    textureReload->pending.wait();
  textureReload.reset();
  if(videoBase) videoBase->cancelled->store(true);
  if(videoWorker.joinable()) videoWorker.join();
  if(videoBase && videoBase->selection.publication.valid()) videoBase->selection.publication.wait();
  if(dmLoader!=nullptr)
    DmLoader_release(dmLoader);
  writeCensus();
  inst=nullptr;
  }

void Resources::startVideoBase() {
  if(inst->dev==nullptr) return;
  std::lock_guard<std::mutex> guard(inst->videoStartSync);
  if(inst->videoBase) return;
  inst->videoBase=std::make_shared<VideoBase>();
  const auto state=inst->videoBase;
  const auto gothic=std::filesystem::path(CommandLine::inst().rootPath());
  const auto mainBase=inst->automaticBase.path;
  inst->videoWorker=std::thread([state,gothic,mainBase] {
    preparingVideo=true;
    try {
      auto selection=BaseStore::resolveVideos(gothic,mainBase,[](std::string_view message) {
        // Background selection diagnostics do not use the application's log callback.
        std::fprintf(stderr,"%.*s\n",int(message.size()),message.data());
        },state->cancelled);
      std::map<std::string,std::filesystem::path> files;
      for(const auto& entry:VideoData::readIndex(selection.path)) files.emplace(entry.source.name,selection.path/entry.file);
      std::lock_guard<std::mutex> guard(state->sync);
      state->selection=std::move(selection);
      state->files=std::move(files);
      state->ready=true;
      }
    catch(const std::exception& error) {
      if(!state->cancelled->load()) std::fprintf(stderr,"video base: preparation failed: %s\n",error.what());
      std::lock_guard<std::mutex> guard(state->sync);
      state->ready=true;
      }
    });
  }

std::unique_ptr<VideoData::Reader> Resources::loadVideo(std::string_view name) {
  startVideoBase();
  std::string key(name);
  for(auto& c:key) c=char(std::toupper(static_cast<unsigned char>(c)));
  std::filesystem::path path;
  bool ready=false;
  if(inst->videoBase) {
    std::lock_guard<std::mutex> guard(inst->videoBase->sync);
    ready=inst->videoBase->ready;
    auto found=inst->videoBase->files.find(key);
    if(found==inst->videoBase->files.end()) found=inst->videoBase->files.find(key+".BIK");
    if(found!=inst->videoBase->files.end()) path=found->second;
    }
  if(path.empty()) {
    if(std::filesystem::path(key).extension().empty()) key+=".BIK";
    // Requested by GOTHIC.DAT, with no source in the supported installation.
    static constexpr std::string_view absentFromInstallation[]={"ADDON_TITLE.BIK"};
    const bool absent=std::find(std::begin(absentFromInstallation),std::end(absentFromInstallation),key)!=std::end(absentFromInstallation);
    const char* reason=absent ? "absent from installation" :
                       ready ? "missing from video base" : "video base is still preparing";
    Log::i("video skipped: \"",name,"\": ",reason);
    if(inst->census) inst->census->missing(key,reason);
    return {};
    }
  return std::make_unique<VideoData::Reader>(path);
  }

void Resources::videoFrameRead(std::string_view name) {
  if(!inst->census || !inst->videoBase) return;
  std::string key(name);
  for(auto& c:key) c=char(std::toupper(static_cast<unsigned char>(c)));
  std::lock_guard<std::mutex> guard(inst->videoBase->sync);
  auto found=inst->videoBase->files.find(key);
  if(found==inst->videoBase->files.end()) found=inst->videoBase->files.find(key+".BIK");
  if(found!=inst->videoBase->files.end())
    inst->census->replaced(found->first,"base/videos/"+found->second.filename().string());
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

Tempest::Device& Resources::device() {
  if(!hasGraphicsDevice())
    throw std::logic_error("graphics device access in headless simulation");
  return *inst->dev;
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

namespace {
const char* scriptDirectory(int language) {
  static constexpr const char* directories[]={"Scripts_EN","Scripts_DE","Scripts_PL","Scripts_RU",
                                               "Scripts_FR","Scripts_ES","Scripts_IT","Scripts_CZ"};
  return language>=0 && language<8 ? directories[language] : "Scripts";
  }
}

zenkit::DaedalusScript Resources::loadBaseProgram(std::string_view request, int language) {
  try {
    std::string_view branch;
    if(inst->basePrograms.findVfs(request,&branch)!=nullptr)
      return loadBaseProgram(request,language,branch);
    const std::string name(request.substr(0,255));
    std::string path="_work/Data/Scripts/_compiled/"+name;
    const ProgramData::Source* source=inst->basePrograms.findHost(path);
    branch="host_ordinary";
    if(source==nullptr && language>=0) {
      path="_work/Data/"+std::string(scriptDirectory(language))+"/_compiled/"+name;
      source=inst->basePrograms.findHost(path);
      branch="host_localized_full";
      }
    if(source==nullptr && language>=0) {
      const auto separator=request.find_last_of("/\\");
      const auto basename=separator==std::string_view::npos ? request : request.substr(separator+1);
      path="_work/Data/"+std::string(scriptDirectory(language))+"/_compiled/"+std::string(basename.substr(0,255));
      source=inst->basePrograms.findHost(path);
      branch="host_localized_basename";
      }
    if(source==nullptr)
      throw std::runtime_error("no source in program base index");
    return loadBaseProgram(request,language,branch,std::filesystem::path(source->path).u16string());
    }
  catch(const BaseStore::Error&) { throw; }
  catch(const std::exception& error) { throw BaseStore::Error("program "+std::string(request)+": "+error.what()); }
  }

bool Resources::loadBaseOutputUnits(std::string_view request, int language,
                                    std::vector<std::shared_ptr<asset::CutsceneBlock>>& blocks) {
  try {
    std::string_view branch;
    if(inst->baseOutputUnits.findVfs(request,&branch)!=nullptr)
      return loadBaseOutputUnits(request,language,branch,blocks);
    const std::string name(request.substr(0,255));
    const OutputUnits::Source* source=inst->baseOutputUnits.findHost("_work/Data/Scripts/content/CUTSCENE/"+name);
    branch="host_ordinary";
    if(source==nullptr && language>=0) {
      source=inst->baseOutputUnits.findHost("_work/Data/"+std::string(scriptDirectory(language))+"/"+name);
      branch="host_localized";
      }
    if(source==nullptr) return false;
    return loadBaseOutputUnits(request,language,branch,blocks,std::filesystem::path(source->path).u16string());
    }
  catch(const BaseStore::Error&) { throw; }
  catch(const std::exception& error) { throw BaseStore::Error("output units "+std::string(request)+": "+error.what()); }
  }

zenkit::DaedalusScript Resources::loadBaseProgram(std::string_view request, int language,
                                                std::string_view branch, std::u16string_view hostPath) {
  try {
    const auto& index = inst->basePrograms;
    const ProgramData::Source* source = nullptr;
    if(hostPath.empty()) {
      std::string_view selectedBranch;
      source = index.findVfs(request,&selectedBranch);
      if(source==nullptr || selectedBranch!=branch)
        throw std::runtime_error("selected VFS source missing from program base index");
      }
    else {
      source = index.findHost(censusSourcePath(std::filesystem::path(hostPath)));
      if(source==nullptr)
        throw std::runtime_error("selected host source missing from program base index");
      }
    const auto file = index.path(*source);
    std::string actualSha256;
    auto program = ProgramData::readProgram(file,&actualSha256);
    if(actualSha256!=source->recordSha256)
      throw std::runtime_error("program record identity differs from base index");
    auto script = zenkit::DaedalusScript::from_program(std::move(program));
    if(inst->census) {
      const auto record = "base/programs/"+file.filename().generic_string();
      inst->census->replaced(request,record);
      inst->census->program("Gothic::loadScript",request,language,branch,record,actualSha256,*source);
      }
    return script;
    }
  catch(const std::exception& error) {
    if(inst->census) inst->census->missing(request,error.what());
    throw BaseStore::Error("program "+std::string(request)+": "+error.what());
    }
  }

bool Resources::loadBaseOutputUnits(std::string_view request, int language, std::string_view branch,
                                   std::vector<std::shared_ptr<asset::CutsceneBlock>>& blocks,
                                   std::u16string_view hostPath) {
  try {
    const auto& index = inst->baseOutputUnits;
    const OutputUnits::Source* source = nullptr;
    if(hostPath.empty()) {
      std::string_view selectedBranch;
      source = index.findVfs(request,&selectedBranch);
      if(source==nullptr || selectedBranch!=branch)
        throw std::runtime_error("selected VFS source missing from output units base index");
      }
    else {
      source = index.findHost(censusSourcePath(std::filesystem::path(hostPath)));
      if(source==nullptr)
        throw std::runtime_error("selected host source missing from output units base index");
      if(!source->skipped.empty())
        return false;
      }
    if(!source->skipped.empty())
      throw std::runtime_error("selected VFS output units source skipped: "+source->skipped);
    const auto file = index.path(*source);
    std::string actualSha256;
    auto library = OutputUnits::readLibrary(file,&actualSha256);
    if(actualSha256!=source->recordSha256)
      throw std::runtime_error("output units record identity differs from base index");
    auto loaded = OutputUnits::sortedBlocks(library);
    blocks = std::move(loaded);
    if(inst->census) {
      const auto record = "base/outputunits/"+file.filename().generic_string();
      inst->census->replaced(request,record);
      inst->census->outputUnits("GameScript::loadDialogOU",request,language,branch,record,actualSha256,*source);
      }
    return true;
    }
  catch(const std::exception& error) {
    if(inst->census) inst->census->missing(request,error.what());
    throw BaseStore::Error("output units "+std::string(request)+": "+error.what());
    }
  }

const char* Resources::renderer() {
  return hasGraphicsDevice() ? device().properties().name : "headless-cpu";
  }

// A bow's string drawn straight back (graphics/mesh/bowdraw): G2 draws the string's middle up and to the
// side, to the arrow its animations hold high over the hand. Copies of the drawn string (S_READY) and of
// its release (S_RELAX) where the middle goes square to the limbs, back as far, to the side only as far as
// the arrow passes the bow; the limbs bend as they do. On a layer of their own: they blend with G2's
static void straightenString(asset::MorphMesh& zmm) {
  const asset::MorphAnimation* ready = nullptr;
  const asset::MorphAnimation* relax = nullptr;
  int32_t                       layer = 0;
  for(auto& a:zmm.animations) {
    if(a.name=="S_READY")
      ready = &a;
    if(a.name=="S_RELAX")
      relax = &a;
    layer = std::max(layer,a.layer);
    }
  const auto& pos = zmm.mesh.positions;
  if(ready==nullptr || ready->vertices.empty() || ready->samples.size()<ready->vertices.size() || pos.empty())
    return;

  // the bow's axes: along the limbs (the longest), across it (the thinnest), back from the grip
  Vec3 lo = Vec3(pos[0].x,pos[0].y,pos[0].z), hi = lo;
  for(auto& p:pos) {
    lo = Vec3(std::min(lo.x,p.x),std::min(lo.y,p.y),std::min(lo.z,p.z));
    hi = Vec3(std::max(hi.x,p.x),std::max(hi.y,p.y),std::max(hi.z,p.z));
    }
  const float ext[3] = {hi.x-lo.x, hi.y-lo.y, hi.z-lo.z};
  int along = 0, across = 0;
  for(int i=1; i<3; ++i) {
    if(ext[i]>ext[along])
      along = i;
    if(ext[i]<ext[across])
      across = i;
    }
  const int back = 3-along-across;
  if(along==across)
    return;
  auto comp = [](const zenkit::Vec3& v, int i) { return i==0 ? v.x : (i==1 ? v.y : v.z); };

  // the string's middle: the vertices drawn the farthest
  const size_t per  = ready->vertices.size();
  float        peak = 0;
  size_t       top  = 0;
  for(size_t k=0; k<per; ++k) {
    auto&       m = ready->samples[k];
    const float d = std::sqrt(m.x*m.x + m.y*m.y + m.z*m.z);
    if(d>peak) {
      peak = d;
      top  = k;
      }
    }
  if(peak<1.f)
    return;
  const float drawn = comp(ready->samples[top],back);
  const float side  = comp(ready->samples[top],across)<0.f ? -BowDraw::StringSide : BowDraw::StringSide;
  if(std::abs(drawn)<1.f)
    return;
  std::unordered_map<uint32_t,float> middle;
  for(size_t k=0; k<per; ++k) {
    auto&       m = ready->samples[k];
    const float d = std::sqrt(m.x*m.x + m.y*m.y + m.z*m.z)/peak;
    const float w = std::clamp((d-0.5f)/0.3f,0.f,1.f);
    if(w>0.f)
      middle[ready->vertices[k]] = w*w*(3.f-2.f*w);
    }

  auto straight = [&](asset::MorphAnimation a, const char* name) {
    a.name  = name;
    a.layer = layer+1;
    const size_t n = a.vertices.size();
    for(size_t i=0; n>0 && i<a.samples.size(); ++i) {
      auto it = middle.find(a.vertices[i%n]);
      if(it==middle.end())
        continue;
      auto&       m  = a.samples[i];
      const float b  = comp(m,back);
      float       to[3] = {};
      to[back]   = b;
      to[across] = side*(b/drawn);
      const float w  = it->second;
      m.x += (to[0]-m.x)*w;
      m.y += (to[1]-m.y)*w;
      m.z += (to[2]-m.z)*w;
      }
    return a;
    };
  auto drawnStr = straight(*ready,BowDraw::StringDrawn);
  auto loose    = relax!=nullptr ? std::make_optional(straight(*relax,BowDraw::StringLoose)) : std::nullopt;
  zmm.animations.push_back(std::move(drawnStr));
  if(loose)
    zmm.animations.push_back(std::move(*loose));
  }

static Sampler implShadowSampler() {
  Tempest::Sampler smp;
  smp.setFiltration(Tempest::Filter::Nearest);
  smp.setClamping(Tempest::ClampMode::ClampToEdge);
  smp.anisotropic = false;
  return smp;
  }

const Sampler& Resources::shadowSampler() {
  static Tempest::Sampler smp = implShadowSampler();
  return smp;
  }


const GthFont& Resources::dialogFont(const float scale) {
  return font("font_old_10_white.tga",FontType::Normal,scale);
  }

const GthFont& Resources::font(const float scale) {
  return font("font_old_10_white.tga",FontType::Normal,scale);
  }

const GthFont& Resources::font(Resources::FontType type, const float scale) {
  return font("font_old_10_white.tga",type,scale);
  }

const GthFont& Resources::font(std::string_view fname, FontType type, const float scale) {
  if(fname.empty())
    return font(scale);
  return inst->implLoadFont(fname, type, scale);
  }

const Texture2d& Resources::fallbackTexture() {
  return inst->fallback;
  }

const Texture2d &Resources::fallbackBlack() {
  return inst->fbZero;
  }

const Tempest::StorageImage& Resources::fallbackImage() {
  return inst->fbImg;
  }

const Tempest::IndexBuffer<uint16_t>& Resources::cubeIbo() {
  return inst->cube;
  }
Tempest::Texture2d* Resources::implLoadTexture(std::string_view cname, bool forceMips, bool pinned) {
  if(isHeadless() || cname.empty())
    return nullptr;

  std::string name(cname);
  if(auto it=texCache.find(name); it!=texCache.end()) {
    if(auto entry=textureRecords.find(it->second.get()); entry!=textureRecords.end() && pinned && !entry->second.pinned) {
      auto& record=entry->second;
      unqueueTexture(record);
      record.pinned=true;
      queueTexture(record);
      }
    return it->second.get();
    }

  const auto probeT = LoadProbe::now();
  TextureData data;
  Texture2d tex;
  try {
    data=implDecodeTexture(cname,forceMips);
    if(!data.pixmap.isEmpty())
      tex=device().texture(data.pixmap,data.useMipmap);
    }
  catch(const std::exception& error) {
    Log::e("textures: unable to upload \"",cname,"\": ",error.what());
    }
  LoadProbe::countTexture(tex,probeT);
  if(!tex.isEmpty()) {
    auto t = std::make_unique<Texture2d>(std::move(tex));
    auto* ret=t.get();
    trackTexture(ret,data,TextureKind::Color,pinned);
    texCache[std::move(name)] = std::move(t);
    return ret;
    }
  texCache[std::move(name)] = nullptr;
  return nullptr;
  }

void Resources::indexHdTextures() {
  const char* dir = std::getenv("OPENGOTHIC_TEXTURES");
  if(dir==nullptr || dir[0]=='\0')
    return;
  std::error_code ec;
  for(auto& e:std::filesystem::directory_iterator(dir,ec)) {
    auto ext = e.path().extension().string();
    for(auto& c:ext)
      c = char(std::toupper(uint8_t(c)));
    if(ext!=".DDS" && ext!=".PNG")
      continue;
    auto key = e.path().stem().string();
    for(auto& c:key)
      c = char(std::toupper(uint8_t(c)));
    // DXT with mipmaps (DDS) over RGBA (PNG): a fraction of the memory
    auto& path = hdTextures[key];
    if(path.empty() || ext==".DDS")
      path = e.path().string();
    }
  if(ec)
    Log::e("textures: unable to read \"",dir,"\"");
  Log::i("textures: ",hdTextures.size()," of our own from \"",dir,"\"");
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

std::filesystem::path Resources::worldOverrideDir(std::string_view world) {
  if(const char* dir = std::getenv("OPENGOTHIC_CONTENT")) {
    if(dir[0]=='\0')
      return {};
    return worldContentDir(dir,world);
    }
  if(const char* home = std::getenv("HOME"))
    return worldContentDir(std::filesystem::path(home)/".local/share/gothic-ai/content",world);
  return {};
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
    } else if(const char* home = std::getenv("HOME")) {
    root = std::filesystem::path(home)/".local/share/gothic-ai/content";
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
    bool valid = !path.empty() && path.find('\0')==std::string::npos &&
                 !relative.is_absolute() && !relative.has_root_name() && FileExt::hasExt(path,"GLB");
    for(const auto& part:relative)
      if(part=="..")
        valid = false;
    if(!valid) {
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

bool Resources::meshReplacementLoaded(std::string_view name, MeshReplacementSource* source) {
  if(inst==nullptr)
    return false;
  std::lock_guard<std::recursive_mutex> guard(inst->sync);
  const auto slash = name.find_last_of("/\\");
  if(slash!=std::string_view::npos)
    name.remove_prefix(slash+1);
  const auto dot = name.find_last_of('.');
  if(dot!=std::string_view::npos)
    name = name.substr(0,dot);
  std::string key(name);
  for(auto& c:key)
    c = char(std::toupper(uint8_t(c)));
  const auto it = inst->meshOverrides.find(key);
  if(it==inst->meshOverrides.end() || !it->second.loaded)
    return false;
  if(source!=nullptr)
    *source = {it->second.file,it->second.sourceSize,it->second.sourceCrc32};
  return true;
  }

asset::StaticMesh Resources::loadVisualMesh(std::string_view name, bool* replaced) {
  std::lock_guard<std::recursive_mutex> guard(inst->sync);
  if(replaced!=nullptr)
    *replaced = false;
  auto original = std::string(name);
  FileExt::exchangeExt(original,"3DS","MRM");
  if(FileExt::hasExt(original,"MRM")) {
    auto key = original.substr(0,original.size()-4);
    for(auto& c:key)
      c = char(std::toupper(uint8_t(c)));
    if(const auto it = inst->meshOverrides.find(key); it!=inst->meshOverrides.end()) {
      it->second.loaded = false;
      try {
        std::vector<uint8_t> bytes;
        auto mesh = loadGltfMesh(it->second.file,&bytes);
        if(inst->census)
          inst->census->replaced(key+".MRM",it->second.replacement);
        if(replaced!=nullptr)
          *replaced = true;
        it->second.sourceSize = bytes.size();
        it->second.sourceCrc32 = uint32_t(mz_crc32(0,bytes.data(),bytes.size()));
        it->second.loaded = true;
        return mesh;
        }
      catch(const std::exception& error) {
        Log::e("meshes: unable to replace \"",original,"\" with \"",it->second.file.string(),"\": ",error.what());
        }
      }
    if(inst->baseMeshes.enabled) {
      if(const auto it = inst->baseMeshes.files.find(key); it!=inst->baseMeshes.files.end()) {
        try {
          auto mesh = readBaseMesh(inst->baseMeshes.dir/it->second);
          if(inst->census)
            inst->census->replaced(key+".MRM","base/meshes/"+it->second);
          return mesh;
          }
        catch(const std::exception& error) {
          Log::e("base: unable to load mesh \"",it->second,"\": ",error.what());
          }
        }
      }
    }
  Log::e("base: unable to load mesh \"",original,"\"");
  if(inst->census) inst->census->missing(original,"mesh unavailable");
  return {};
  }

void Resources::indexMaterialMaps() {
  std::filesystem::path root;
  if(const char* dir = std::getenv("OPENGOTHIC_CONTENT")) {
    if(dir[0]=='\0')
      return;
    root = dir;
    } else if(const char* home = std::getenv("HOME")) {
    root = std::filesystem::path(home)/".local/share/gothic-ai/content";
    } else {
    return;
    }
  root /= "materials";

  std::ifstream file(root/"materials.json", std::ios::binary);
  if(!file)
    return;
  const std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
  rapidjson::Document table;
  table.Parse<rapidjson::kParseValidateEncodingFlag>(text.data(), text.size());
  if(table.HasParseError() || !table.IsObject()) {
    Log::e("materials: invalid materials.json in \"",root.string(),"\"");
    return;
    }

  for(auto& entry:table.GetObject()) {
    if(!entry.value.IsObject()) {
      Log::e("materials: invalid entry \"",entry.name.GetString(),"\"");
      continue;
      }
    MaterialMapFiles maps;
    auto path = [&](const char* field, std::string& out, std::string* replacement = nullptr) {
      const auto it = entry.value.FindMember(field);
      if(it==entry.value.MemberEnd())
        return true;
      if(!it->value.IsString())
        return false;
      const std::string name(it->value.GetString(), it->value.GetStringLength());
      const auto relative = std::filesystem::path(name);
      if(name.empty() || name.find('\0')!=std::string::npos || relative.is_absolute() || !FileExt::hasExt(name,"DDS"))
        return false;
      for(const auto& component:relative)
        if(component=="..")
          return false;
      out = (root/relative).lexically_normal().string();
      if(replacement!=nullptr)
        *replacement = (std::filesystem::path("materials")/relative).lexically_normal().generic_string();
      return true;
      };
    bool valid = path("albedo", maps.albedo, &maps.albedoReplacement) && path("normal", maps.normal) && path("orm", maps.orm);
    if(const auto mode = entry.value.FindMember("antitile"); mode!=entry.value.MemberEnd()) {
      if(!mode->value.IsString()) {
        valid = false;
        } else {
        const std::string_view value(mode->value.GetString(), mode->value.GetStringLength());
        if(value=="offset")
          maps.antitile = Material::Antitile::Offset;
        else if(value=="rotate")
          maps.antitile = Material::Antitile::Rotate;
        else if(value!="off")
          valid = false;
        }
      }
    if(const auto cell = entry.value.FindMember("antitile_cell"); cell!=entry.value.MemberEnd()) {
      if(!cell->value.IsNumber()) {
        valid = false;
        } else {
        const auto value = cell->value.GetDouble();
        if(!std::isfinite(value) || value<=0 || value>std::numeric_limits<float>::max() ||
           float(value)<=0 || !std::isfinite(1.f/float(value)))
          valid = false;
        else
          maps.antitileCell = float(value);
        }
      }
    if(const auto scale = entry.value.FindMember("scale"); scale!=entry.value.MemberEnd()) {
      if(!scale->value.IsNumber()) {
        valid = false;
        } else {
        const auto value = scale->value.GetDouble();
        if(!std::isfinite(value) || std::abs(value)>std::numeric_limits<float>::max())
          valid = false;
        else
          maps.scale = float(value);
        }
      }
    if(!valid) {
      Log::e("materials: invalid entry \"",entry.name.GetString(),"\"");
      continue;
      }
    std::string key(entry.name.GetString(), entry.name.GetStringLength());
    for(auto& c:key)
      c = char(std::toupper(uint8_t(c)));
    materialMaps[std::move(key)] = std::move(maps);
    }
  materialMapsPresent = !materialMaps.empty();
  Log::i("materials: ",materialMaps.size()," entries from \"",root.string(),"\"");
  }

const Texture2d* Resources::implLoadMaterialTexture(const std::string& path, TextureKind kind) {
  if(path.empty())
    return nullptr;
  const auto key = (kind==TextureKind::Normal ? "normal:" : kind==TextureKind::Orm ? "orm:" : "color:")+path;
  if(auto it = materialTextureCache.find(key); it!=materialTextureCache.end())
    return it->second.get();
  std::unique_ptr<Texture2d> texture;
  const auto probeT = LoadProbe::now();
  try {
    TextureData data{Pixmap(path)};
    if(data.pixmap.format()!=TextureFormat::DXT5 && (kind==TextureKind::Normal || data.pixmap.format()!=TextureFormat::DXT1)) {
      Log::e("materials: unsupported DDS format in \"",path,"\"");
      } else {
      data.useMipmap=data.pixmap.mipCount()>1;
      data.source=path;
      texture=std::make_unique<Texture2d>(device().texture(data.pixmap,data.useMipmap));
      trackTexture(texture.get(),data,kind,false);
      }
    }
  catch(...) {
    Log::e("materials: unable to load \"",path,"\"");
    }
  const auto ret = texture.get();
  if(ret!=nullptr)
    LoadProbe::countTexture(*ret,probeT);
  materialTextureCache.emplace(key,std::move(texture));
  return ret;
  }

Texture2d Resources::implLoadTextureUncached(std::string_view name, bool forceMips) {
  auto decoded=implDecodeTexture(name,forceMips);
  try {
    return decoded.pixmap.isEmpty() ? Texture2d() : device().texture(decoded.pixmap,decoded.useMipmap);
    }
  catch(const std::exception& error) {
    Log::e("textures: unable to upload \"",name,"\": ",error.what());
    return {};
    }
  }

Resources::TextureData Resources::implDecodeTexture(std::string_view name, bool forceMips) {
  if(isHeadless() || name.empty())
    return {};

  if(!hdTextures.empty() && FileExt::hasExt(name,"TGA")) {
    std::string key(name.substr(0,name.size()-4));
    for(auto& c:key)
      c = char(std::toupper(uint8_t(c)));
    if(auto it = hdTextures.find(key); it!=hdTextures.end()) {
      try {
        TextureData decoded{Pixmap(it->second),true,it->second};
        if(census)
          census->replaced(name,it->second);
        return decoded;
        }
      catch(...) {
        Log::e("textures: unable to load \"",it->second,"\"");
        }
      }
    }

  if(baseTextures.enabled && FileExt::hasExt(name,"TGA")) {
    std::string key(name.substr(0,name.size()-4));
    for(auto& c:key)
      c = char(std::toupper(static_cast<unsigned char>(c)));
    if(const auto it = baseTextures.files.find(key); it!=baseTextures.files.end()) {
      try {
        auto decoded=decodeBaseTexture(baseTextures.dir/it->second,forceMips);
        if(!decoded.pixmap.isEmpty()) {
          if(census)
            census->replaced(name,"base/textures/"+it->second);
          return decoded;
          }
        Log::e("base: unable to load texture \"",it->second,"\"");
        }
      catch(const std::exception& error) {
        Log::e("base: unable to load texture \"",it->second,"\": ",error.what());
        }
      }
    }

  Log::e("base: unable to load texture \"",name,"\"");
  if(census) census->missing(name,"texture unavailable");
  return {};
  }

Resources::TextureData Resources::decodeBaseTexture(const std::filesystem::path& path, bool forceMips) {
  if(FileExt::hasExt(path.string(),"PNG"))
    return {Tempest::Pixmap(path.string()),true,path}; // device.texture(pm) generates mips for non-DXT originals.
  TextureData decoded{Tempest::Pixmap(path.string()),false,path};
  decoded.useMipmap = forceMips || decoded.pixmap.mipCount()>1;
  return decoded;
  }

bool Resources::TextureRecord::wanted() const {
  return pinned || std::any_of(demand.begin(),demand.end(),[](uint32_t count) { return count!=0; });
  }

void Resources::trackTexture(Texture2d* texture, const TextureData& data, TextureKind kind, bool pinned) {
  TextureRecord record;
  record.texture=texture;
  record.source=data.source;
  record.format=texture->format();
  record.kind=kind;
  record.mipmaps=data.useMipmap;
  record.pinned=pinned;
  record.bytes=LoadProbe::textureBytes(*texture);
  auto entry=textureRecords.emplace(texture,std::move(record)).first;
  queueTexture(entry->second);
  }

void Resources::unqueueTexture(TextureRecord& record) {
  textureIdle.erase({record.lastDemand,reinterpret_cast<uintptr_t>(record.texture)});
  textureMissing.erase(&record);
  }

void Resources::queueTexture(TextureRecord& record) {
  if(record.wanted()) {
    if(!record.resident && !record.failed)
      textureMissing.insert(&record);
    } else if(record.resident) {
    textureIdle.emplace(std::make_pair(record.lastDemand,reinterpret_cast<uintptr_t>(record.texture)),&record);
    }
  }

void Resources::configureTextures(std::string_view budget) {
  if(inst==nullptr)
    return;
  const auto* enabled=std::getenv("OPENGOTHIC_TEXTURE_STREAMING");
  const auto* budgetEnv=std::getenv("OPENGOTHIC_TEXTURE_BUDGET_MB");
  const auto configured=budgetEnv!=nullptr ? std::string_view(budgetEnv) : budget;
  int64_t megabytes=256;
  if(!configured.empty()) {
    std::string value(configured);
    char* end=nullptr;
    megabytes=std::strtoll(value.c_str(),&end,10);
    if(end==value.c_str() || *end!='\0' || megabytes<0 || megabytes>std::numeric_limits<int64_t>::max()/1048576)
      throw std::runtime_error("texture_budget_mb must be a nonnegative integer");
    }
  std::lock_guard<std::recursive_mutex> guard(inst->sync);
  inst->textureBudget=uint64_t(megabytes)*1048576;
  inst->textureStreaming=enabled==nullptr || std::strcmp(enabled,"0")!=0;
  inst->textureOverBudget=false;
  }

Resources::TextureStats Resources::textureStats() {
  if(inst==nullptr)
    return {};
  std::lock_guard<std::recursive_mutex> guard(inst->sync);
  TextureStats stats;
  for(const auto& queue:inst->recycled)
    stats.retiredBytes+=queue.textureBytes;
  stats.liveBytes=Texture2d::liveBytes();
  stats.count=Texture2d::liveCount();
  stats.peakBytes=Texture2d::peakBytes();
  stats.evicted=inst->texturesEvicted;
  stats.reloaded=inst->texturesReloaded;
  stats.categoryBytes[size_t(TextureCategory::Other)]=stats.liveBytes;
  stats.categoryCount[size_t(TextureCategory::Other)]=stats.count;
  for(const auto& entry:inst->textureRecords) {
    const auto& record=entry.second;
    if(!record.resident || record.category==TextureCategory::Other)
      continue;
    const auto bytes=record.bytes;
    stats.categoryBytes[size_t(record.category)]+=bytes;
    stats.categoryCount[size_t(record.category)]+=1;
    stats.categoryBytes[size_t(TextureCategory::Other)]-=bytes;
    stats.categoryCount[size_t(TextureCategory::Other)]-=1;
    }
  return stats;
  }

uint64_t Resources::textureGeneration() {
  if(inst==nullptr)
    return 0;
  std::lock_guard<std::recursive_mutex> guard(inst->sync);
  return inst->textureEpoch;
  }

bool Resources::texturesReady() {
  if(inst==nullptr || inst->dev==nullptr)
    return true;
  std::lock_guard<std::recursive_mutex> guard(inst->sync);
  return inst->textureMissing.empty();
  }

TextureFormat Resources::textureFormat(const Texture2d* texture) {
  if(texture==nullptr)
    return TextureFormat::Undefined;
  if(inst==nullptr)
    return texture->format();
  std::lock_guard<std::recursive_mutex> guard(inst->sync);
  if(auto record=inst->textureRecords.find(texture); record!=inst->textureRecords.end())
    return record->second.format;
  return texture->format();
  }

void Resources::textureDemand(const Material& material, TextureCategory category, bool add) {
  if(inst==nullptr || inst->dev==nullptr)
    return;
  std::lock_guard<std::recursive_mutex> guard(inst->sync);
  std::vector<const Texture2d*> textures=material.frames;
  textures.insert(textures.end(),{material.tex,material.normal,material.orm});
  std::sort(textures.begin(),textures.end(),std::less<const Texture2d*>());
  textures.erase(std::unique(textures.begin(),textures.end()),textures.end());
  bool unmatched=false;
  for(const auto* texture:textures) {
    auto entry=inst->textureRecords.find(texture);
    if(entry==inst->textureRecords.end())
      continue;
    auto& record=entry->second;
    auto& count=record.demand[size_t(category)];
    if(!add && count==0) {
      if(!unmatched) {
        Log::e("textures: demand released without an owner");
        unmatched=true;
        }
      assert(count!=0 && "texture demand released without an owner");
      continue;
      }
    inst->unqueueTexture(record);
    if(add) {
      ++count;
      record.lastDemand=++inst->textureClock;
      } else {
      --count;
      record.lastDemand=++inst->textureClock;
      }
    for(size_t i=0;i<record.demand.size();++i)
      if(record.demand[i]!=0) {
        record.category=TextureCategory(i);
        break;
        }
    inst->queueTexture(record);
    }
  }

void Resources::retireTexture(Texture2d&& texture) {
  if(texture.isEmpty())
    return;
  auto& queue=recycled[recycledId];
  queue.textureBytes+=LoadProbe::textureBytes(texture);
  texture.markRetired();
  queue.textures.emplace_back(std::move(texture));
  }

void Resources::publishTexture() {
  const auto reload=textureReload;
  if(!reload || reload->ready.isEmpty())
    return;
  auto& record=textureRecords.at(reload->identity);
  unqueueTexture(record);
  if(record.wanted()) {
    *record.texture=std::move(reload->ready);
    record.bytes=LoadProbe::textureBytes(*record.texture);
    record.resident=true;
    texturePayloadChanged=true;
    ++texturesReloaded;
    }
  queueTexture(record);
  textureReload.reset();
  }

void Resources::updateTextures(ChunkResidency* residency) {
  FrameProfile::Scope timing(FrameProfile::Zone::TextureUpdate);
  if(inst==nullptr || inst->dev==nullptr)
    return;
  std::lock_guard<std::recursive_mutex> guard(inst->sync);
  auto& resources=*inst;
  auto liveBytes=Texture2d::liveBytes();
  const auto finish=[&] {
    if(resources.texturePayloadChanged) {
      ++resources.textureEpoch;
      resources.texturePayloadChanged=false;
      }
    if(liveBytes<=resources.textureBudget) {
      resources.textureOverBudget=false;
      } else if(resources.textureStreaming && resources.textureIdle.empty() && !resources.textureOverBudget) {
      Log::i("textures: budget exceeded with no unrequested resident payloads (",liveBytes," > ",resources.textureBudget," bytes)");
      resources.textureOverBudget=true;
      }
    };
  if(!resources.textureReload && resources.textureMissing.empty() &&
     (!resources.textureStreaming || liveBytes<=resources.textureBudget || resources.textureIdle.empty())) {
    finish();
    return;
    }
  const auto deadline=ChunkResidency::Clock::now()+ChunkResidency::WorkBudget;

  if(resources.textureReload) {
    const auto reload=resources.textureReload;
    if(reload->pending.valid() && reload->pending.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
      try {
        reload->ready=reload->pending.get();
        }
      catch(const std::exception& error) {
        auto& record=resources.textureRecords.at(reload->identity);
        Log::e("textures: unable to reload \"",record.source.string(),"\": ",error.what());
        resources.unqueueTexture(record);
        record.failed=true;
        resources.queueTexture(record);
        resources.textureReload.reset();
        }
      }
    if(resources.textureReload && !reload->ready.isEmpty() && reload->queued.expired()) {
      if(residency==nullptr) {
        resources.publishTexture();
        } else {
        auto ticket=std::make_shared<int>(0);
        reload->queued=ticket;
        residency->enqueue([reload,ticket](ChunkResidency::Clock::time_point deadline) {
          if(ChunkResidency::Clock::now()>=deadline)
            return false;
          if(Resources::inst!=nullptr) {
            std::lock_guard<std::recursive_mutex> guard(Resources::inst->sync);
            if(Resources::inst->textureReload==reload)
              Resources::inst->publishTexture();
            }
          return true;
          });
        }
      }
    }

  if(!resources.textureReload && !resources.textureMissing.empty()) {
    const auto& record=**resources.textureMissing.begin();
    auto reload=std::make_shared<TextureReload>();
    reload->identity=record.texture;
    const auto path=record.source;
    const auto mipmaps=record.mipmaps;
    auto* device=resources.dev;
    reload->pending=std::async(std::launch::async,[path,mipmaps,device] {
      const auto started=LoadProbe::now();
      Pixmap pixmap(path.string());
      auto texture=device->texture(pixmap,mipmaps);
      if(texture.isEmpty())
        throw std::runtime_error("texture reload returned an empty image");
      LoadProbe::countTexture(texture,started);
      return texture;
      });
    resources.textureReload=std::move(reload);
    }

  liveBytes=Texture2d::liveBytes();
  size_t evictions=0;
  while(resources.textureStreaming && liveBytes>resources.textureBudget && !resources.textureIdle.empty() &&
        evictions<16 && ChunkResidency::Clock::now()<deadline) {
    auto* record=resources.textureIdle.begin()->second;
    assert(record->resident && !record->wanted());
    auto& neutral=resources.texturePlaceholders[size_t(record->kind)];
    if(neutral.isEmpty()) {
      Pixmap placeholder(1,1,TextureFormat::RGBA8);
      const std::array<uint8_t,4> color=record->kind==TextureKind::Normal ? std::array<uint8_t,4>{128,128,255,128} :
                                        record->kind==TextureKind::Orm ? std::array<uint8_t,4>{255,255,0,255} :
                                                                       std::array<uint8_t,4>{255,255,255,255};
      std::memcpy(placeholder.data(),color.data(),color.size());
      neutral=resources.dev->texture(placeholder,false);
      }
    auto replacement=neutral.shared();
    resources.unqueueTexture(*record);
    resources.retireTexture(std::move(*record->texture));
    *record->texture=std::move(replacement);
    record->resident=false;
    resources.texturePayloadChanged=true;
    ++resources.texturesEvicted;
    ++evictions;
    liveBytes=Texture2d::liveBytes();
    }
  finish();
  }

ProtoMesh* Resources::implLoadMesh(std::string_view name) {
  if(name.size()==0)
    return nullptr;

  auto cname = std::string(name);
  auto it    = aniMeshCache.find(cname);
  if(it!=aniMeshCache.end())
    return it->second.get();

  auto  t   = implLoadMeshMain(cname);
  auto  ret = t.get();
  aniMeshCache[cname] = std::move(t);
  if(ret==nullptr)
    Log::e("unable to load mesh \"",cname,"\"");
  return ret;
  }

bool Resources::replaceModelMesh(asset::ModelMesh& model, std::unique_ptr<Skeleton>& skeleton,
                                 std::string_view visual, std::string_view meshFile) {
  auto key = std::string(visual.substr(0,visual.find_last_of('.')));
  for(auto& c:key)
    c = char(std::toupper(uint8_t(c)));
  const auto it = meshOverrides.find(key);
  if(it==meshOverrides.end())
    return false;

  it->second.loaded = false;
  try {
    if(model.attachments.empty())
      throw std::runtime_error("model has no rigid attachments");
    if(!model.meshes.empty())
      throw std::runtime_error("model has soft-skin meshes");

    // Stage every attachment before touching the original geometry. A model
    // with one attachment retains the whole-scene, name-independent path.
    std::vector<uint8_t> bytes;
    std::unordered_map<std::string,asset::StaticMesh> replacements;
    if(model.attachments.size()==1)
      replacements.emplace(model.attachments.begin()->first,loadGltfMesh(it->second.file,&bytes));
    else {
      auto nodes = loadGltfMeshNodes(it->second.file,&bytes);
      for(auto& node:nodes) {
        if(node.name.empty())
          throw std::runtime_error("mesh node has an empty attachment name");
        if(std::none_of(model.attachments.begin(),model.attachments.end(),
                        [&](const auto& attachment) { return attachment.first==node.name; }))
          throw std::runtime_error("unknown mesh node attachment name: " + node.name);
        if(!replacements.emplace(node.name,std::move(node.mesh)).second)
          throw std::runtime_error("duplicate mesh node attachment name: " + node.name);
        }
      for(const auto& attachment:model.attachments)
        if(replacements.find(attachment.first)==replacements.end())
          throw std::runtime_error("missing mesh node for attachment: " + attachment.first);
      }

    // Direct MDM/ASC loading historically has no skeleton. Read its paired
    // hierarchy only for an indexed replacement, and retain it only on success.
    std::unique_ptr<Skeleton> paired;
    if(skeleton==nullptr) {
      auto mdhName = std::string(meshFile);
      FileExt::assignExt(mdhName,"MDH");
      asset::ModelHierarchy mdh {};
      if(!loadHierarchyData(mdhName,mdh))
        throw std::runtime_error("model has no bind hierarchy");
      paired.reset(new Skeleton(mdh,nullptr,visual));
      }
    const auto& sk = skeleton==nullptr ? *paired : *skeleton;
    for(auto& attachment:replacements) {
      const auto node = sk.findNode(attachment.first);
      if(node==size_t(-1))
        throw std::runtime_error("attachment node is missing from bind hierarchy");
      const auto& bind = sk.tr[node];
      for(size_t col=0;col<4;++col)
        for(size_t row=0;row<4;++row)
          if(!std::isfinite(bind[col][row]))
            throw std::runtime_error("non-finite attachment bind transform");
      if(bind[0][3]!=0 || bind[1][3]!=0 || bind[2][3]!=0 || bind[3][3]!=1)
        throw std::runtime_error("attachment bind transform must be affine");
      const double determinant =
          double(bind[0][0])*(double(bind[1][1])*bind[2][2]-double(bind[2][1])*bind[1][2]) -
          double(bind[1][0])*(double(bind[0][1])*bind[2][2]-double(bind[2][1])*bind[0][2]) +
          double(bind[2][0])*(double(bind[0][1])*bind[1][2]-double(bind[1][1])*bind[0][2]);
      if(!std::isfinite(determinant) || determinant==0)
        throw std::runtime_error("singular attachment bind transform");
      auto inverse = bind;
      inverse.inverse();

      auto& mesh = attachment.second;
      for(auto& p:mesh.positions) {
        inverse.project(p.x,p.y,p.z);
        if(!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
          throw std::runtime_error("non-finite local attachment position");
        }
      // Inverse-transpose of the model-to-local transform is bind^T.
      auto normal = [&bind](zenkit::Vec3& n) {
        const double x = double(bind[0][0])*n.x + double(bind[0][1])*n.y + double(bind[0][2])*n.z;
        const double y = double(bind[1][0])*n.x + double(bind[1][1])*n.y + double(bind[1][2])*n.z;
        const double z = double(bind[2][0])*n.x + double(bind[2][1])*n.y + double(bind[2][2])*n.z;
        const double length = std::hypot(x,y,z);
        if(!std::isfinite(length) || length==0)
          throw std::runtime_error("non-finite or zero local attachment normal");
        n = {float(x/length),float(y/length),float(z/length)};
        };
      for(auto& n:mesh.normals)
        normal(n);
      for(auto& sub:mesh.sub_meshes) {
        for(auto& wedge:sub.wedges)
          normal(wedge.normal);
        if(determinant<0)
          for(auto& triangle:sub.triangles)
            std::swap(triangle.wedges[1],triangle.wedges[2]);
        }
      mesh.bbox.min = mesh.bbox.max = mesh.positions.front();
      for(const auto& p:mesh.positions)
        for(unsigned axis=0;axis<3;++axis) {
          mesh.bbox.min[axis] = std::min(mesh.bbox.min[axis],p[axis]);
          mesh.bbox.max[axis] = std::max(mesh.bbox.max[axis],p[axis]);
          }
      for(unsigned axis=0;axis<3;++axis) {
        mesh.obbox.center[axis] = float((double(mesh.bbox.min[axis])+mesh.bbox.max[axis])*0.5);
        mesh.obbox.half_width[axis] = float((double(mesh.bbox.max[axis])-mesh.bbox.min[axis])*0.5);
        }
      mesh.obbox.axes[0] = {1,0,0};
      mesh.obbox.axes[1] = {0,1,0};
      mesh.obbox.axes[2] = {0,0,1};
      }

    for(auto& attachment:model.attachments)
      attachment.second = std::move(replacements.at(attachment.first));
    if(paired!=nullptr)
      skeleton = std::move(paired);
    if(census)
      census->replaced(meshFile,it->second.replacement);
    it->second.sourceSize = bytes.size();
    it->second.sourceCrc32 = uint32_t(mz_crc32(0,bytes.data(),bytes.size()));
    it->second.loaded = true;
    return true;
    }
  catch(const std::exception& error) {
    Log::e("meshes: unable to replace \"",meshFile,"\" with \"",it->second.file.string(),"\": ",error.what());
    return false;
    }
  }

std::unique_ptr<ProtoMesh> Resources::implLoadMeshMain(std::string name) {
  if(FileExt::hasExt(name,"3DS")) {
    FileExt::exchangeExt(name,"3DS","MRM");

    bool replaced = false;
    auto zmsh = loadVisualMesh(name,&replaced);

    if(zmsh.sub_meshes.empty())
      return nullptr;

    PackedMesh packed(zmsh,PackedMesh::PK_Visual,replaced);
    return std::unique_ptr<ProtoMesh>{new ProtoMesh(std::move(packed),name)};
    }

  if(FileExt::hasExt(name,"MMS") || FileExt::hasExt(name,"MMB")) {
    FileExt::exchangeExt(name,"MMS","MMB");

    auto zmm = loadMorphData(name);

    if(zmm.mesh.sub_meshes.empty())
      return nullptr;
    straightenString(zmm);

    PackedMesh packed(zmm.mesh,PackedMesh::PK_VisualMorph);
    std::unique_ptr<ProtoMesh> ret{new ProtoMesh(std::move(packed),zmm.animations,name)};
    // the vertex each morph moves the most, where it rests: the string's middle of a bow
    for(size_t i=0; i<ret->morph.size() && i<zmm.animations.size(); ++i) {
      auto&       a    = zmm.animations[i];
      auto&       m    = ret->morph[i];
      const auto  per  = a.vertices.size();
      float       best = -1;
      for(size_t k=0; per>0 && k<a.samples.size(); ++k) {
        auto&       sm = a.samples[k];
        const float d  = sm.x*sm.x + sm.y*sm.y + sm.z*sm.z;
        const auto  id = a.vertices[k%per];
        if(d<=best || id>=zmm.mesh.positions.size())
          continue;
        best       = d;
        auto& at   = zmm.mesh.positions[id];
        m.peakAt   = Tempest::Vec3(at.x,at.y,at.z);
        m.peakMove = Tempest::Vec3(sm.x,sm.y,sm.z);
        }
      }
    return ret;
    }

  if(FileExt::hasExt(name,"MDS")) {
    auto anim = Resources::loadAnimation(name);
    if(anim==nullptr)
      return nullptr;

    auto mesh   = std::string(anim->defaultMesh());

    FileExt::exchangeExt(mesh,nullptr,"MDM") ||
    FileExt::exchangeExt(mesh,"ASC",  "MDM");

    auto mdhName = std::string(anim->defaultMesh());
    if(mdhName.empty())
      mdhName = name;
    FileExt::assignExt(mdhName,"MDH");

    asset::ModelHierarchy hierarchy {};
    if(loadHierarchyData(mdhName,hierarchy)) {
      // Find a MDH hirarchy file and separate mesh
      std::unique_ptr<Skeleton> sk{new Skeleton(hierarchy,anim,name)};
      std::unique_ptr<ProtoMesh> t;

      asset::ModelMesh own {};
      if(loadModelMeshData(mesh,own)) {
        const bool replaced = replaceModelMesh(own,sk,name,mesh);
        t.reset(new ProtoMesh(own,std::move(sk),name,replaced));
        }
      else
        t.reset(new ProtoMesh(hierarchy,std::move(sk),name));

      return t;
      }

    // MDL files contain both the hirarchy and mesh, try to find one as a fallback
    FileExt::exchangeExt(mdhName,"MDH","MDL");
    asset::Model model {};
    if(loadModelData(mdhName,model)) {
      std::unique_ptr<Skeleton> sk{new Skeleton(model.hierarchy,anim,name)};
      const bool replaced = replaceModelMesh(model.mesh,sk,name,mdhName);
      std::unique_ptr<ProtoMesh> t{new ProtoMesh(model.hierarchy,model.mesh,std::move(sk),name,replaced)};
      return t;
      }
    }

  if(FileExt::hasExt(name,"MDM") || FileExt::hasExt(name,"ASC")) {
    FileExt::exchangeExt(name,"ASC","MDM");

    asset::ModelMesh own {};
    if(!loadModelMeshData(name,own))
      return nullptr;

    std::unique_ptr<Skeleton> sk;
    const bool replaced = replaceModelMesh(own,sk,name,name);
    std::unique_ptr<ProtoMesh> t{new ProtoMesh(own,std::move(sk),name,replaced)};
    return t;
    }

  if(FileExt::hasExt(name,"MDL")) {
    asset::Model model {};
    if(!loadModelData(name,model))
      return nullptr;

    std::unique_ptr<Skeleton> sk{new Skeleton(model.hierarchy,nullptr,name)};
    const bool replaced = replaceModelMesh(model.mesh,sk,name,name);
    std::unique_ptr<ProtoMesh> t{new ProtoMesh(model.hierarchy,model.mesh,std::move(sk),name,replaced)};
    return t;
    }

  if(FileExt::hasExt(name,"TGA")) {
    Log::e("decals should be loaded by Resources::implDecalMesh instead");
    }

  return nullptr;
  }

PfxEmitterMesh* Resources::implLoadEmiterMesh(std::string_view name) {
  // TODO: reuse code from Resources::implLoadMeshMain
  auto cname = std::string(name);
  auto it    = emiMeshCache.find(cname);
  if(it!=emiMeshCache.end())
    return it->second.get();

  auto& ret = emiMeshCache[cname];

  if(FileExt::hasExt(cname,"3DS")) {
    FileExt::exchangeExt(cname,"3DS","MRM");

    bool replaced = false;
    auto zmsh = loadVisualMesh(cname,&replaced);

    if(zmsh.sub_meshes.empty())
      return nullptr;

    PackedMesh packed(zmsh,PackedMesh::PK_Visual,replaced);
    ret = std::unique_ptr<PfxEmitterMesh>(new PfxEmitterMesh(packed));
    return ret.get();
    }

  if(FileExt::hasExt(name,"MDM")) {
    asset::ModelMesh mesh {};
    if(!loadModelMeshData(cname,mesh))
      return nullptr;
    ret = std::unique_ptr<PfxEmitterMesh>(new PfxEmitterMesh(mesh));
    return ret.get();
    }

  return nullptr;
  }

ProtoMesh* Resources::implDecalMesh(const asset::VisualDecal& decal) {
  DecalK key;
  key.mat         = Material(decal);
  key.sX          = decal.dimension.x;
  key.sY          = decal.dimension.y;
  key.decal2Sided = decal.two_sided;

  if(key.mat.tex==nullptr && !isHeadless())
    return nullptr;

  auto it = decalMeshCache.find(key);
  if(it!=decalMeshCache.end())
    return it->second.get();

  Resources::Vertex vbo[8] = {
    {{-1.f, -1.f, 0.f},{0,0,-1},{0,1}, 0xFFFFFFFF},
    {{ 1.f, -1.f, 0.f},{0,0,-1},{1,1}, 0xFFFFFFFF},
    {{ 1.f,  1.f, 0.f},{0,0,-1},{1,0}, 0xFFFFFFFF},
    {{-1.f,  1.f, 0.f},{0,0,-1},{0,0}, 0xFFFFFFFF},

    {{-1.f, -1.f, 0.f},{0,0, 1},{0,1}, 0xFFFFFFFF},
    {{ 1.f, -1.f, 0.f},{0,0, 1},{1,1}, 0xFFFFFFFF},
    {{ 1.f,  1.f, 0.f},{0,0, 1},{1,0}, 0xFFFFFFFF},
    {{-1.f,  1.f, 0.f},{0,0, 1},{0,0}, 0xFFFFFFFF},
    };
  for(auto& i:vbo) {
    i.pos[0]*=key.sX;
    i.pos[1]*=key.sY;
    }

  std::vector<Resources::Vertex> cvbo(vbo,vbo+8);
  std::vector<uint32_t>          cibo;
  if(key.decal2Sided)
    cibo = { 0,1,2, 0,2,3, 4,6,5, 4,7,6 }; else
    cibo = { 0,1,2, 0,2,3 };

  std::unique_ptr<ProtoMesh> t{new ProtoMesh(key.mat, std::move(cvbo), std::move(cibo))};

  auto ret = t.get();
  decalMeshCache[key] = std::move(t);
  return ret;
  }

std::unique_ptr<Animation> Resources::implLoadAnimation(std::string name) {
  if(name.size()<4)
    return nullptr;

  if(Gothic::inst().version().game==2)
    FileExt::exchangeExt(name,"MDS","MSB");

  if(FileExt::hasExt(name,"MSB") || FileExt::hasExt(name,"MDS")) {
    asset::ModelScript own {};
    if(!loadModelScriptData(name,own))
      return nullptr;
    return std::unique_ptr<Animation>{new Animation(own,name.substr(0,name.size()-4),false)};
    }

  return nullptr;
  }

std::vector<uint8_t> Resources::readMusicRecord(std::string_view provider, std::string_view request,
                                                const char* kind, const MusicData::Source& source) {
  try {
    if(source.kind!=kind)
      throw std::runtime_error("music data: requested kind differs from indexed source: "+std::string(request));
    const auto& outcome = provider=="gk" ? source.gk : source.dx8;
    auto bytes = MusicData::readRecord(baseMusic,outcome.record);
    if(census) census->musicLoaded(provider,request,source,outcome);
    return bytes;
    }
  catch(const std::exception& error) {
    if(census) census->missing(request,error.what());
    try { Log::e("base: ",error.what()); } catch(...) {}
    throw;
    }
  }

DmResult Resources::readGkMusic(std::string_view name, const char* kind, std::vector<uint8_t>& bytes) noexcept {
  bytes.clear();
  try {
    const auto* source = baseMusic.findGk(name);
    if(source==nullptr)
      return DmResult_NOT_FOUND;
    if(source->gk.failure) {
      static constexpr std::array<std::string_view,7> results = {
        "SUCCESS","INVALID_ARGUMENT","INVALID_STATE","MEMORY_EXHAUSTED","NOT_FOUND","FILE_CORRUPT","MUTEX_ERROR"
        };
      for(size_t i=1;i<results.size();++i)
        if(*source->gk.failure==results[i])
          return static_cast<DmResult>(i);
      return DmResult_FILE_CORRUPT;
      }
    bytes = readMusicRecord("gk",name,kind,*source);
    return DmResult_SUCCESS;
    }
  catch(const std::bad_alloc&) { return DmResult_MEMORY_EXHAUSTED; }
  catch(...) { return DmResult_FILE_CORRUPT; }
  }

std::vector<uint8_t> Resources::readDxMusic(const std::vector<std::u16string>& paths, const char16_t* name,
                                          const char* kind) {
  for(const auto& path:paths) {
    auto file=std::filesystem::path(path)/name;
    if(file.is_absolute()) file=file.lexically_relative(std::filesystem::path(CommandLine::inst().rootPath()));
    const auto* source=baseMusic.findDx8(file.lexically_normal().generic_string());
    if(source==nullptr) continue;
    if(source->dx8.failure)
      throw std::runtime_error(*source->dx8.failure);
    return readMusicRecord("dx8",Tempest::TextCodec::toUtf8(name),kind,*source);
    }
  if(census) census->missing(Tempest::TextCodec::toUtf8(name),"music data: missing from base index");
  throw std::runtime_error("file not found");
  }

Dx8::PatternList Resources::implLoadDxMusic(std::string_view name) {
  if(isHeadless())
    return Dx8::PatternList();
  auto u = Tempest::TextCodec::toUtf16(name);
  return dxMusic->load(u.c_str());
  }

DmSegment* Resources::implLoadMusicSegment(char const* name) {
  if(isHeadless())
    return nullptr;
  DmSegment* sgt = nullptr;
  DmResult rv = DmLoader_getSegment(dmLoader, name, &sgt);
  if(rv != DmResult_SUCCESS) {
    Log::e("Music segment not found: ", name);
    return nullptr;
    }
  return sgt;
  }

Tempest::Sound Resources::implLoadSoundBuffer(std::string_view name) {
  if(name.empty())
    return Tempest::Sound();

  auto& external = Gothic::inst().externalSoundFn;
  if(!(external && external(name,fBuff))) {
    const auto* source = baseAudio.find(name);
    if(source==nullptr)
      return Tempest::Sound();
    try {
      AudioData::readPayload(baseAudio,*source,name,fBuff);
      }
    catch(const std::exception& error) {
      Log::e("base: ",error.what());
      if(census) census->missing(name,error.what());
      return Tempest::Sound();
      }
    if(census) census->audioLoaded(name,*source);
    }
  try {
    Tempest::MemReader rd(fBuff.data(),fBuff.size());
    return Tempest::Sound(rd);
    }
  catch(...) {
    auto cname = std::string (name);
    Log::e("unable to load sound \"",cname,"\"");
    return Tempest::Sound();
    }
  }

GthFont &Resources::implLoadFont(std::string_view name, FontType type, const float scale) {
  if(isHeadless())
    throw std::logic_error("font access in headless simulation");
  std::lock_guard<std::recursive_mutex> g(inst->syncFont);

  auto key   = FontK(std::string(name), type, scale);
  auto it    = gothicFnt.find(key);
  if(it!=gothicFnt.end())
    return *(*it).second;

  std::string_view file = name;
  for(size_t i=0; i<name.size();++i) {
    if(name[i]=='.') {
      file = name.substr(0, i);
      break;
      }
    }

  string_frm tex, fnt;
  switch(type) {
    case FontType::Normal:
    case FontType::Disabled:
    case FontType::Yellow:
    case FontType::Red:
      tex = string_frm(file,".tga");
      fnt = string_frm(file,".fnt");
      break;
    case FontType::Hi:
      tex = string_frm(file,"_hi.tga");
      fnt = string_frm(file,"_hi.fnt");
      break;
    }

  auto color = Tempest::Color(1.f);
  switch(type) {
    case FontType::Normal:
    case FontType::Hi:
      color = Tempest::Color(1.f);
      break;
    case FontType::Disabled:
      color = Tempest::Color(1.f,1.f,1.f,0.6f);
      break;
    case FontType::Yellow:
      color = Tempest::Color(1.f,1.f,0.1f,1.f);
      //color = Tempest::Color(0.81f,0.78f,0.01f,1.f);
      break;
    case FontType::Red:
      color = Tempest::Color(1.f,0.f,0.f,1.f);
      break;
    }

  std::string fntKey(fnt.begin(),fnt.end());
  for(auto& c:fntKey)
    c = char(std::toupper(static_cast<unsigned char>(c)));
  const auto entry = baseFonts.files.find(fntKey);
  if(entry == baseFonts.files.end()) {
    Log::e("failed to open resource: ", fnt);
    if(census) census->missing(fnt,"missing from base index");
    // throw std::runtime_error("failed to open resource: " + std::string{fnt});
    auto ptr   = std::make_unique<GthFont>();
    GthFont* f = ptr.get();
    gothicFnt[key] = std::move(ptr);
    return *f;
    }

  asset::Font data;
  try {
    data = FontData::readFont(baseFonts.dir/entry->second);
    }
  catch(const std::exception& error) {
    if(census) census->missing(fnt,error.what());
    throw;
    }
  const auto* texture = Resources::loadTexture(tex);
  auto ptr   = std::make_unique<GthFont>(std::move(data),texture,color);
  GthFont* f = ptr.get();
  f->setScale(scale);
  if(census) census->replaced(fnt,"base/fonts/"+entry->second);
  gothicFnt[key] = std::move(ptr);
  return *f;
  }

Texture2d Resources::loadTextureUncached(std::string_view name, bool forceMips) {
  return inst->implLoadTextureUncached(name, forceMips);
  }

const Texture2d* Resources::loadTexture(std::string_view name, bool forceMips, bool pinned) {
  std::lock_guard<std::recursive_mutex> g(inst->sync);
  return inst->implLoadTexture(name,forceMips,pinned);
  }

const Texture2d* Resources::loadTexture(Tempest::Color color) {
  if(isHeadless() || color==Color())
    return nullptr;
  std::lock_guard<std::recursive_mutex> g(inst->sync);
  auto& cache = inst->pixCache;
  auto it = cache.find(color);
  if(it!=cache.end())
    return it->second.get();

  uint8_t iv[4] = { uint8_t(255.f*color.r()), uint8_t(255.f*color.g()), uint8_t(255.f*color.b()), uint8_t(255.f*color.a()) };
  Pixmap p2(1,1,TextureFormat::RGBA8);
  std::memcpy(p2.data(),iv,4);

  auto t       = std::make_unique<Texture2d>(device().texture(p2));
  auto ret     = t.get();
  cache[color] = std::move(t);
  return ret;
  }

bool Resources::hasTextureVariant(std::string_view name) {
  return name.find("_V0")!=std::string_view::npos || name.find("_C0")!=std::string_view::npos;
  }

std::string Resources::textureVariantName(std::string_view name, int32_t iv, int32_t ic) {
  std::string ret(name);
  // Preserve the original long-name fallback and first-tag substitution.
  if(name.size()>=128)
    return ret;
  if(const auto at = ret.find("_V0"); at!=std::string::npos)
    ret.replace(at+1,2,"V"+std::to_string(iv));
  if(const auto at = ret.find("_C0"); at!=std::string::npos)
    ret.replace(at+1,2,"C"+std::to_string(ic));
  return ret;
  }

const Texture2d *Resources::loadTexture(std::string_view name, int32_t iv, int32_t ic) {
  return loadMaterialAlbedo(textureVariantName(name,iv,ic));
  }

bool Resources::hasTexture(std::string_view name) const {
  if(!FileExt::hasExt(name,"TGA")) return false;
  std::string key(name.substr(0,name.size()-4));
  for(auto& c:key) c=char(std::toupper(static_cast<unsigned char>(c)));
  return hdTextures.count(key)!=0 || baseTextures.files.count(key)!=0;
  }

std::vector<const Texture2d*> Resources::loadTextureAnim(std::string_view name, bool pinned) {
  std::vector<const Texture2d*> ret;
  if(name.find("_A0")==std::string::npos &&
     name.find("_a0")==std::string::npos)
    return ret;

  for(int id=0; ; ++id) {
    size_t at = 0;
    char   buf[128]={};
    for(size_t i=0;i<name.size();++i) {
      if(i+2<name.size() && name[i]=='_' && (name[i+1]=='A' || name[i+1]=='a') && name[i+2]=='0'){
        at += size_t(std::snprintf(buf+at,sizeof(buf)-at,"_A%d",id));
        i+=2;
        } else {
        buf[at] = name[i];
        if('a'<=buf[at] && buf[at]<='z')
          buf[at] = char(buf[at]+'A'-'a');
        at++;
        }
      if(at>sizeof(buf))
        return ret;
      }
   auto t = inst->hasTexture(buf) ? loadTexture(buf,false,pinned) : nullptr;
    if(t==nullptr) {
      string_frm buf2(buf,".TGA");
      t = inst->hasTexture(buf2) ? loadTexture(buf2,false,pinned) : nullptr;
      if(t==nullptr)
        return ret;
      }
    ret.push_back(t);
    }
  }

Texture2d Resources::loadTexturePm(const Pixmap &pm) {
  if(isHeadless())
    return Texture2d();
  if(pm.isEmpty()) {
    Pixmap p2(1,1,TextureFormat::R8);
    std::memset(p2.data(),0,1);
    return device().texture(p2);
    }
  return device().texture(pm);
  }

Material Resources::loadMaterial(const asset::Material& src, bool enableAlphaTest, bool requireAlphaTest) {
  return Material(src,enableAlphaTest,requireAlphaTest);
  }

Resources::MaterialMaps Resources::loadMaterialMaps(std::string_view name) {
  if(isHeadless() || name.empty())
    return {};
  std::lock_guard<std::recursive_mutex> g(inst->sync);
  if(inst->materialMaps.empty())
    return {};
  std::string key(name);
  for(auto& c:key)
    c = char(std::toupper(uint8_t(c)));
  const auto it = inst->materialMaps.find(key);
  if(it==inst->materialMaps.end())
    return {};
  const auto& files = it->second;
  MaterialMaps ret;
  ret.albedo = inst->implLoadMaterialTexture(files.albedo,TextureKind::Color);
  ret.normal = inst->implLoadMaterialTexture(files.normal,TextureKind::Normal);
  ret.orm    = inst->implLoadMaterialTexture(files.orm,TextureKind::Orm);
  ret.scale  = files.scale;
  ret.antitileCell = files.antitileCell;
  ret.antitile = files.antitile;
  ret.present = true;
  ret.hasAlbedo = !files.albedo.empty();
  return ret;
  }

const Texture2d* Resources::loadMaterialAlbedo(std::string_view name) {
  const auto maps = loadMaterialMaps(name);
  if(!maps.hasAlbedo)
    return loadTexture(name,false,false);
  // A broken replacement must not silently open the original texture.
  if(maps.albedo==nullptr)
    return &fallbackTexture();
  materialAlbedoReplaced(name);
  return maps.albedo;
  }

void Resources::materialAlbedoReplaced(std::string_view name) {
  std::lock_guard<std::recursive_mutex> g(inst->sync);
  if(!inst->census)
    return;
  std::string key(name);
  for(auto& c:key)
    c = char(std::toupper(uint8_t(c)));
  const auto it = inst->materialMaps.find(key);
  if(it!=inst->materialMaps.end() && !it->second.albedoReplacement.empty())
    inst->census->replaced(name,it->second.albedoReplacement);
  }

bool Resources::materialMapsEnabled() {
  return inst!=nullptr && inst->materialMapsPresent;
  }

const ProtoMesh* Resources::loadMesh(std::string_view name) {
  if(name.size()==0)
    return nullptr;
  std::lock_guard<std::recursive_mutex> g(inst->sync);
  return inst->implLoadMesh(name);
  }

const PfxEmitterMesh* Resources::loadEmiterMesh(std::string_view name) {
  if(name.empty())
    return nullptr;
  std::lock_guard<std::recursive_mutex> g(inst->sync);
  return inst->implLoadEmiterMesh(name);
  }

const Skeleton* Resources::loadSkeleton(std::string_view name) {
  auto s = Resources::loadMesh(name);
  if(s==nullptr)
    return nullptr;
  return s->skeleton.get();
  }

const Animation* Resources::loadAnimation(std::string_view name) {
  auto cname = std::string(name);

  std::lock_guard<std::recursive_mutex> g(inst->sync);
  auto& cache = inst->animCache;
  auto it=cache.find(cname);
  if(it!=cache.end())
    return it->second.get();
  auto t       = inst->implLoadAnimation(cname);
  auto ret     = t.get();
  cache[cname] = std::move(t);
  return ret;
  }

std::filesystem::path Resources::baseModelFile(std::string_view name) {
  const auto separator = name.find_last_of("/\\");
  std::string key(separator==std::string_view::npos ? name : name.substr(separator+1));
  for(auto& c:key)
    c = char(std::toupper(static_cast<unsigned char>(c)));
  const auto found = inst->baseModels.files.find(key);
  if(found==inst->baseModels.files.end())
    return {};
  return inst->baseModels.dir/found->second;
  }

bool Resources::loadHierarchyData(std::string_view name, asset::ModelHierarchy& out) {
  const auto file = baseModelFile(name);
  if(file.empty())
    return false;
  try {
    out = ModelData::readHierarchy(file);
    }
  catch(const std::exception& error) {
    if(inst->census) inst->census->missing(name,error.what());
    throw;
    }
  if(inst->census) inst->census->replaced(name,"base/models/"+file.filename().string());
  return true;
  }

bool Resources::loadModelMeshData(std::string_view name, asset::ModelMesh& out) {
  const auto file = baseModelFile(name);
  if(file.empty())
    return false;
  try {
    out = ModelData::readMesh(file);
    }
  catch(const std::exception& error) {
    if(inst->census) inst->census->missing(name,error.what());
    throw;
    }
  if(inst->census) inst->census->replaced(name,"base/models/"+file.filename().string());
  return true;
  }

bool Resources::loadModelData(std::string_view name, asset::Model& out) {
  const auto file = baseModelFile(name);
  if(file.empty())
    return false;
  try {
    out = ModelData::readModel(file);
    }
  catch(const std::exception& error) {
    if(inst->census) inst->census->missing(name,error.what());
    throw;
    }
  if(inst->census) inst->census->replaced(name,"base/models/"+file.filename().string());
  return true;
  }

bool Resources::loadModelScriptData(std::string_view name, asset::ModelScript& out) {
  const auto file = baseModelFile(name);
  if(file.empty())
    return false;
  try {
    out = ModelData::readScript(file);
    }
  catch(const std::exception& error) {
    if(inst->census) inst->census->missing(name,error.what());
    throw;
    }
  if(inst->census) inst->census->replaced(name,"base/models/"+file.filename().string());
  return true;
  }

bool Resources::loadAnimationData(std::string_view name, asset::ModelAnimation& out) {
  const auto file = baseModelFile(name);
  if(file.empty())
    return false;
  try {
    out = ModelData::readAnimation(file);
    }
  catch(const std::exception& error) {
    if(inst->census) inst->census->missing(name,error.what());
    throw;
    }
  if(inst->census) inst->census->replaced(name,"base/models/"+file.filename().string());
  return true;
  }

asset::MorphMesh Resources::loadMorphData(std::string_view name) {
  const auto file = baseModelFile(name);
  if(file.empty()) {
    if(inst->census) inst->census->missing(name,"missing from base index");
    throw std::runtime_error("failed to open resource: " + std::string(name));
    }
  asset::MorphMesh out;
  try {
    out = ModelData::readMorph(file);
    }
  catch(const std::exception& error) {
    if(inst->census) inst->census->missing(name,error.what());
    throw;
    }
  if(inst->census) inst->census->replaced(name,"base/models/"+file.filename().string());
  return out;
  }

Tempest::Sound Resources::loadSoundBuffer(std::string_view name) {
  std::lock_guard<std::recursive_mutex> g(inst->sync);
  return inst->implLoadSoundBuffer(name);
  }

Dx8::PatternList Resources::loadDxMusic(std::string_view name) {
  std::lock_guard<std::recursive_mutex> g(inst->sync);
  return inst->implLoadDxMusic(name);
  }

DmSegment* Resources::loadMusicSegment(char const* name) {
  std::lock_guard<std::recursive_mutex> g(inst->sync);
  return inst->implLoadMusicSegment(name);
  }

const ProtoMesh* Resources::decalMesh(const asset::VisualDecal& decal) {
  std::lock_guard<std::recursive_mutex> g(inst->sync);
  return inst->implDecalMesh(decal);
  }

const Resources::VobTree* Resources::loadVobBundle(std::string_view name) {
  std::lock_guard<std::recursive_mutex> g(inst->sync);
  return inst->implLoadVobBundle(name);
  }

void Resources::resetRecycled(uint8_t fId) {
  std::lock_guard<std::recursive_mutex> g(inst->sync);
  inst->recycledId = fId;
  inst->recycled[fId].ssbo.clear();
  inst->recycled[fId].img.clear();
  inst->recycled[fId].att.clear();
  inst->recycled[fId].zb.clear();
  inst->recycled[fId].arr.clear();
  inst->recycled[fId].rtas.clear();
  inst->recycled[fId].lifetime.clear();
  inst->recycled[fId].textures.clear();
  inst->recycled[fId].textureBytes=0;
  }

void Resources::recycle(Tempest::DescriptorArray &&arr) {
  if(arr.isEmpty())
    return;
  std::lock_guard<std::recursive_mutex> g(inst->sync);
  inst->recycled[inst->recycledId].arr.emplace_back(std::move(arr));
  }

void Resources::recycle(Tempest::StorageBuffer&& ssbo) {
  if(ssbo.isEmpty())
    return;
  std::lock_guard<std::recursive_mutex> g(inst->sync);
  inst->recycled[inst->recycledId].ssbo.emplace_back(std::move(ssbo));
  }

void Resources::recycle(Tempest::StorageImage&& img) {
  if(img.isEmpty())
    return;
  std::lock_guard<std::recursive_mutex> g(inst->sync);
  inst->recycled[inst->recycledId].img.emplace_back(std::move(img));
  }

void Resources::recycle(Tempest::Attachment&& img) {
  if(img.isEmpty())
    return;
  std::lock_guard<std::recursive_mutex> g(inst->sync);
  inst->recycled[inst->recycledId].att.emplace_back(std::move(img));
  }

void Resources::recycle(Tempest::ZBuffer&& img) {
  if(img.isEmpty())
    return;
  std::lock_guard<std::recursive_mutex> g(inst->sync);
  inst->recycled[inst->recycledId].zb.emplace_back(std::move(img));
  }

void Resources::recycle(Tempest::AccelerationStructure&& rtas) {
  if(rtas.isEmpty())
    return;
  std::lock_guard<std::recursive_mutex> g(inst->sync);
  inst->recycled[inst->recycledId].rtas.emplace_back(std::move(rtas));
  }

void Resources::recycle(std::shared_ptr<void>&& lifetime) {
  if(!lifetime)
    return;
  std::lock_guard<std::recursive_mutex> g(inst->sync);
  inst->recycled[inst->recycledId].lifetime.emplace_back(std::move(lifetime));
  }

const Resources::VobTree* Resources::implLoadVobBundle(std::string_view filename) {
  auto cname = std::string(filename);
  auto i     = zenCache.find(cname);
  if(i!=zenCache.end())
    return i->second.get();

  std::vector<std::shared_ptr<asset::VirtualObject>> bundle;
  try {
    WorldData::ZenFile base;
    if(loadBaseZen(cname,"world",base)) {
      bundle = std::move(base.vobs);

      }
    }
  catch(...) {
    Log::e("unable to load Zen-file: \"",cname,"\"");
    }

  auto ret = zenCache.insert(std::make_pair(filename,std::make_unique<VobTree>(std::move(bundle))));
  return ret.first->second.get();
  }

const AttachBinder *Resources::bindMesh(const ProtoMesh &anim, const Skeleton &s) {
  std::lock_guard<std::recursive_mutex> g(inst->sync);

  if(anim.submeshId.size()==0){
    static AttachBinder empty;
    return &empty;
    }
  BindK k = BindK(&s,&anim);

  auto it = inst->bindCache.find(k);
  if(it!=inst->bindCache.end())
    return it->second.get();

  std::unique_ptr<AttachBinder> ret(new AttachBinder(s,anim));
  auto p = ret.get();
  inst->bindCache[k] = std::move(ret);
  return p;
  }

Tempest::VertexBuffer<Resources::Vertex> Resources::sphere(int passCount, float R){
  std::vector<Resources::Vertex> r;
  r.reserve( size_t(4*pow(3, passCount+1)) );

  static const double pi = 3.141592654;

  Resources::Vertex v1 = {
    {1, 0, -0.5},
    {0,0,0},{0,0},0
    };
  Resources::Vertex v2 = {
    {float(cos(2*pi/3)), float(sin(2*pi/3)), -0.5},
    {0,0,0},{0,0},0
    };
  Resources::Vertex v3 = {
    {float(cos(2*pi/3)), -float(sin(2*pi/3)), -0.5},
    {0,0,0},{0,0},0
    };

  Resources::Vertex v4 = {
    {0, 0, 0.5},
    {0,0,0},{0,0},0
    };

  r.push_back(v1);
  r.push_back(v3);
  r.push_back(v2);

  r.push_back(v1);
  r.push_back(v2);
  r.push_back(v4);

  r.push_back(v2);
  r.push_back(v3);
  r.push_back(v4);

  r.push_back(v1);
  r.push_back(v4);
  r.push_back(v3);

  for(size_t i=0; i<r.size(); ++i){
    Resources::Vertex& v = r[i];
    float l = std::sqrt(v.pos[0]*v.pos[0] + v.pos[1]*v.pos[1] + v.pos[2]*v.pos[2]);

    v.pos[0] /= l;
    v.pos[1] /= l;
    v.pos[2] /= l;
    }

  for(int c=0; c<passCount; ++c){
    size_t maxI = r.size();
    for( size_t i=0; i<maxI; i+=3 ){
      Resources::Vertex x = {
        {
          0.5f*(r[i].pos[0]+r[i+1].pos[0]),
          0.5f*(r[i].pos[1]+r[i+1].pos[1]),
          0.5f*(r[i].pos[2]+r[i+1].pos[2])
        },
        {0,0,0},{0,0},0
      };
      Resources::Vertex y = {
        {
          0.5f*(r[i+2].pos[0]+r[i+1].pos[0]),
          0.5f*(r[i+2].pos[1]+r[i+1].pos[1]),
          0.5f*(r[i+2].pos[2]+r[i+1].pos[2])
        },
        {0,0,0},{0,0},0
      };
      Resources::Vertex z = {
        {
          0.5f*(r[i].pos[0]+r[i+2].pos[0]),
          0.5f*(r[i].pos[1]+r[i+2].pos[1]),
          0.5f*(r[i].pos[2]+r[i+2].pos[2])
        },
        {0,0,0},{0,0},0
      };

      r.push_back( r[i] );
      r.push_back( x );
      r.push_back( z );

      r.push_back( x );
      r.push_back( r[i+1] );
      r.push_back( y );

      r.push_back( y );
      r.push_back( r[i+2] );
      r.push_back( z );

      r[i]   = x;
      r[i+1] = y;
      r[i+2] = z;
      }

    for(size_t i=0; i<r.size(); ++i){
      Resources::Vertex & v = r[i];
      float l = std::sqrt(v.pos[0]*v.pos[0] + v.pos[1]*v.pos[1] + v.pos[2]*v.pos[2]);

      v.pos[0] /= l;
      v.pos[1] /= l;
      v.pos[2] /= l;
      }
    }

  for(size_t i=0; i<r.size(); ++i){
    Resources::Vertex & v = r[i];
    v.norm[0] = v.pos[0];
    v.norm[1] = v.pos[1];
    v.norm[2] = v.pos[2];

    v.pos[0] *= R;
    v.pos[1] *= R;
    v.pos[2] *= R;
    }

  return device().vbo(r);
  }

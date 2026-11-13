#include "resources.h"
#include "gameservices.h"
#include "resources_internal.h"
#include "loadprobe.h"
#include "basestore.h"
#include "commandline.h"
#include "musicnative.h"
#include "videodata.h"

#include <Tempest/MemReader>
#include <Tempest/File>
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
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <cstdio>

#include "graphics/mesh/submesh/pfxemittermesh.h"
#include "graphics/mesh/submesh/packedmesh.h"
#include "graphics/mesh/facerig.h"
#include "graphics/mesh/renderrig.h"
#include "graphics/mesh/skeleton.h"
#include "graphics/mesh/protomesh.h"
#include "graphics/mesh/animation.h"
#include "graphics/mesh/attachbinder.h"
#include "graphics/mesh/bowdraw.h"
#include "graphics/material.h"
#include "dmusic/directmusic.h"
#include "utils/fileext.h"
#include "utils/gthfont.h"

#include "contentcensus.h"
#include "gltfmesh.h"
#include "basemesh.h"
#include "modeldata.h"
#include "fontdata.h"
#include "worlddata.h"
#include "utils/string_frm.h"

#include <dmusic.h>
#include <optional>
#include <unordered_map>

using namespace Tempest;
using ResourcesInternal::modelKey;

namespace ModelData { asset::ModelScript readScriptOverlay(const std::filesystem::path& file); }

Resources* Resources::inst=nullptr;

struct Resources::VideoBase {
  std::shared_ptr<std::atomic_bool> cancelled=std::make_shared<std::atomic_bool>(false);
  std::mutex sync;
  BaseStore::Selection selection;
  std::map<std::string,std::filesystem::path> files;
  bool ready=false;
  };

namespace ResourcesInternal {
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

const char* scriptDirectory(int language) {
  static constexpr const char* directories[]={"Scripts_EN","Scripts_DE","Scripts_PL","Scripts_RU",
                                               "Scripts_FR","Scripts_ES","Scripts_IT","Scripts_CZ"};
  return language>=0 && language<8 ? directories[language] : "Scripts";
  }
}

Resources::Resources() {
  inst=this;
  configureTextures();
  initCensus();
  indexMeshOverrides();
  indexCharacterHeads();
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
  indexCharacterHeads();

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

  fbImg   = device.image2d(TextureFormat::R32U,1,1);
  fbImg3d = device.image3d(TextureFormat::RGBA8,1,1,1);
  fbPageTbl = device.image3d(TextureFormat::R32U,1,1,1);

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

Resources::~Resources() {
  for(auto& reload:textureReloads)
    if(reload->pending.valid())
      reload->pending.wait();
  textureReloads.clear();
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

Tempest::Device& Resources::device() {
  if(!hasGraphicsDevice())
    throw std::logic_error("graphics device access in headless simulation");
  return *inst->dev;
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
  auto comp = [](const asset::Vec3& v, int i) { return i==0 ? v.x : (i==1 ? v.y : v.z); };

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

const Tempest::StorageImage& Resources::fallbackImage3d() {
  return inst->fbImg3d;
  }

const Tempest::StorageImage& Resources::fallbackPageTable() {
  return inst->fbPageTbl;
  }

const Tempest::IndexBuffer<uint16_t>& Resources::cubeIbo() {
  return inst->cube;
  }
const ProtoMesh* Resources::loadCharacterGroom(std::string_view instance) {
  if(!CommandLine::inst().doHeroHead())
    return nullptr;
  std::string key(instance);
  for(auto& c:key)
    c=char(std::toupper(uint8_t(c)));
  std::lock_guard<std::recursive_mutex> guard(inst->sync);
  const auto it=inst->characterHeads.find(key);
  if(it==inst->characterHeads.end() || it->second.groom.empty())
    return nullptr;
  auto& entry=it->second;
  if(!entry.groomAttempted) {
    entry.groomAttempted=true;
    try {
      // OPENGOTHIC_GROOM_SHARE=0.25: a quarter of the strands, four times wider (the cost of strands, a far head)
      const char* env=std::getenv("OPENGOTHIC_GROOM_SHARE");
      const float share=env!=nullptr ? float(std::atof(env)) : 1.f;
      PackedMesh packed(entry.groom,share>0.f ? share : 1.f);
      const size_t meshlets=packed.indices.size()/PackedMesh::MaxInd;
      entry.groomMesh=std::make_unique<ProtoMesh>(std::move(packed),entry.groom.generic_string(),false);
      Log::i("characters: loaded strands for ",key,": ",meshlets," meshlets");
      }
    catch(const std::exception& error) {
      Log::e("characters: unable to load strands for ",key," from \"",entry.groom.string(),"\": ",error.what());
      }
    }
  return entry.groomMesh.get();
  }

const ProtoMesh* Resources::loadCharacterHead(std::string_view instance) {
  if(!CommandLine::inst().doHeroHead())
    return nullptr;
  std::string key(instance);
  for(auto& c:key)
    c=char(std::toupper(uint8_t(c)));
  // any script instance the table lists: the hero and the npcs with heads of their own
  std::lock_guard<std::recursive_mutex> guard(inst->sync);
  const auto it=inst->characterHeads.find(key);
  if(it==inst->characterHeads.end())
    return nullptr;
  auto& entry=it->second;
  if(!entry.attempted) {
    entry.attempted=true;
    try {
      auto head=std::make_unique<GltfHeadMesh>(loadGltfHeadMesh(entry.file));
      // PK_VisualMorph retains source IDs; this does not install MMB animations.
      PackedMesh packed(head->mesh,PackedMesh::PK_VisualMorph,true);
      head->renderVertexIds=std::move(packed.verticesId);
      auto mesh=std::make_unique<ProtoMesh>(std::move(packed),entry.file.generic_string());
      Log::i("characters: loaded head for ",key,": ",head->parts.size()," parts, ",
             head->mesh.positions.size()," vertices, ",head->targets.size()," targets");
      if(!entry.face.empty()) {
        try {
          std::ifstream file(entry.face,std::ios::binary);
          if(!file)
            throw std::runtime_error("unable to open the file");
          const std::string text{std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
          mesh->face=FaceProfile::parse(text,*head);
          Log::i("characters: face profile for ",key,": ",mesh->face->anims.size()," anims, ",mesh->face->cues.size()," cues, ",
                  mesh->face->leaks.size()," leaks");
          }
        catch(const std::exception& error) {
          Log::e("characters: invalid face profile for ",key," in \"",entry.face.string(),"\": ",error.what(),
                 "; the head shows no face anims");
          }
        }
      mesh->head=std::move(head);
      entry.mesh=std::move(mesh);
      }
    catch(const std::exception& error) {
      Log::e("characters: unable to load head for ",key," from \"",entry.file.string(),"\": ",error.what());
      }
    }
  return entry.mesh.get();
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
  LoadProbe::Vobs::Scope probe("mesh_read");
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

ProtoMesh* Resources::implLoadMesh(std::string_view name) {
  LoadProbe::Requests::add(0,name);
  LoadProbe::Vobs::Scope probe("mesh_hit");
  if(name.size()==0)
    return nullptr;

  auto cname = std::string(name);
  auto it    = aniMeshCache.find(cname);
  if(LoadProbe::enabled())
    LoadProbe::Vobs::mesh(cname,it!=aniMeshCache.end());
  if(it!=aniMeshCache.end())
    return it->second.get();

  probe.rename("mesh_miss");
  LoadProbe::Startup::Scope startupProbe(LoadProbe::Startup::Model);
  auto  t   = implLoadMeshMain(cname);
  auto  ret = t.get();
  if(LoadProbe::enabled() && ret) LoadProbe::Startup::Scope::bytes(LoadProbe::Startup::Model,ret->probeCpuBytes());
  aniMeshCache[cname] = std::move(t);
  if(ret==nullptr)
    Log::e("unable to load mesh \"",cname,"\"");
  return ret;
  }

bool Resources::replaceModelMesh(asset::ModelMesh& model, std::unique_ptr<Skeleton>& skeleton,
                                 std::string_view visual, std::string_view meshFile) {
  LoadProbe::Vobs::Scope probe("mesh_read");
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
      auto normal = [&bind](asset::Vec3& n) {
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

  auto& external = GameServices::inst().externalSoundFn;
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

Material Resources::loadMaterial(const asset::Material& src, bool enableAlphaTest, bool requireAlphaTest) {
  return Material(src,enableAlphaTest,requireAlphaTest);
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

const RenderRig* Resources::loadRenderRig(std::string_view name) {
  std::lock_guard<std::recursive_mutex> g(inst->sync);
  return inst->renderRigs ? inst->renderRigs->rig(name) : nullptr;
  }

const RenderRig* Resources::renderRigOfMesh(std::string_view mesh) {
  std::lock_guard<std::recursive_mutex> g(inst->sync);
  return inst->renderRigs ? inst->renderRigs->rigOfMesh(mesh) : nullptr;
  }

std::string Resources::bodyOfVariant(std::string_view body, int32_t variant) {
  std::lock_guard<std::recursive_mutex> g(inst->sync);
  return inst->renderRigs ? inst->renderRigs->bodyOf(body,variant) : std::string();
  }

const Animation* Resources::loadAnimation(std::string_view name) {
  LoadProbe::Requests::add(1,name);
  auto cname = std::string(name);

  std::lock_guard<std::recursive_mutex> g(inst->sync);
  auto& cache = inst->animCache;
  auto it=cache.find(cname);
  if(it!=cache.end())
    return it->second.get();
  LoadProbe::Startup::Scope startupProbe(LoadProbe::Startup::Animation);
  auto t       = inst->implLoadAnimation(cname);
  auto ret     = t.get();
  if(LoadProbe::enabled() && ret) LoadProbe::Startup::Scope::bytes(LoadProbe::Startup::Animation,ret->probeCpuBytes());
  cache[cname] = std::move(t);
  return ret;
  }

std::filesystem::path Resources::baseModelFile(std::string_view name) {
  const auto found = inst->baseModels.files.find(modelKey(name));
  if(found==inst->baseModels.files.end())
    return {};
  return inst->baseModels.dir/found->second;
  }

template<class T>
bool Resources::loadModelRecord(std::string_view name, T& out, T (*read)(const std::filesystem::path&)) {
  LoadProbe::Vobs::Scope probe("mesh_read");
  const auto content = inst->contentModels.find(modelKey(name));
  if(content!=inst->contentModels.end()) {
    try {
      auto record = read(content->second);
      if constexpr(std::is_same_v<T,asset::ModelAnimation>) {
        if(record.frame_count==0 || record.node_count==0 || record.fps<=0 ||
           record.node_indices.size()!=record.node_count ||
           uint64_t(record.frame_count)*record.node_count!=record.samples.size() ||
           std::any_of(record.node_indices.begin(),record.node_indices.end(),[](uint32_t node) {
             return node>=MAX_NUM_SKELETAL_NODES;
             }))
          throw std::runtime_error("invalid animation frames, nodes, samples or fps");
        }
      out = std::move(record);
      if(inst->census) inst->census->replaced(name,content->second.generic_string());
      return true;
      }
    catch(const std::exception& error) {
      Log::e("content models: skipping \"",content->second.string(),"\": ",error.what());
      }
    }
  const auto file = baseModelFile(name);
  if(file.empty())
    return false;
  try {
    out = read(file);
    }
  catch(const std::exception& error) {
    if(inst->census) inst->census->missing(name,error.what());
    throw;
    }
  if(inst->census) inst->census->replaced(name,"base/models/"+file.filename().string());
  return true;
  }

bool Resources::loadHierarchyData(std::string_view name, asset::ModelHierarchy& out) {
  LoadProbe::Startup::Scope startupProbe(LoadProbe::Startup::Skeleton);
  return loadModelRecord(name,out,ModelData::readHierarchy);
  }

bool Resources::loadModelMeshData(std::string_view name, asset::ModelMesh& out) {
  return loadModelRecord(name,out,ModelData::readMesh);
  }

bool Resources::loadModelData(std::string_view name, asset::Model& out) {
  return loadModelRecord(name,out,ModelData::readModel);
  }

bool Resources::loadModelScriptData(std::string_view name, asset::ModelScript& out) {
  LoadProbe::Vobs::Scope probe("mesh_read");
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
  const auto content = inst->contentModels.find(modelKey(name));
  if(content!=inst->contentModels.end()) {
    try {
      auto overlay = ModelData::readScriptOverlay(content->second);
      for(const auto& animation:overlay.animations)
        if(std::any_of(out.aliases.begin(),out.aliases.end(),[&](const auto& alias) { return alias.name==animation.name; }))
          throw std::runtime_error("overlay animation changes kind: "+animation.name);
      for(const auto& alias:overlay.aliases)
        if(std::any_of(out.animations.begin(),out.animations.end(),[&](const auto& animation) { return animation.name==alias.name; }))
          throw std::runtime_error("overlay alias changes kind: "+alias.name);
      const auto merge = [](auto& base, auto& additions) {
        for(auto& entry:additions) {
          const auto found = std::find_if(base.begin(),base.end(),[&](const auto& old) {
            return old.name==entry.name;
            });
          if(found==base.end()) base.push_back(std::move(entry));
          else *found = std::move(entry);
          }
        };
      merge(out.animations,overlay.animations);
      merge(out.aliases,overlay.aliases);
      if(inst->census) inst->census->replaced(name,content->second.generic_string());
      }
    catch(const std::exception& error) {
      Log::e("content models: skipping \"",std::filesystem::absolute(content->second).string(),"\": ",error.what());
      }
    }
  return true;
  }

bool Resources::loadAnimationData(std::string_view name, asset::ModelAnimation& out) {
  LoadProbe::Requests::add(1,name);
  return loadModelRecord(name,out,ModelData::readAnimation);
  }

asset::MorphMesh Resources::loadMorphData(std::string_view name) {
  asset::MorphMesh out;
  if(!loadModelRecord(name,out,ModelData::readMorph)) {
    if(inst->census) inst->census->missing(name,"missing from base index");
    throw std::runtime_error("failed to open resource: " + std::string(name));
    }
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

const Resources::VobTree* Resources::loadVobBundle(std::string_view name, bool (*readLayer)(VobTree&,std::string_view)) {
  std::lock_guard<std::recursive_mutex> g(inst->sync);
  return inst->implLoadVobBundle(name,readLayer);
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

const Resources::VobTree* Resources::implLoadVobBundle(std::string_view filename, bool (*readLayer)(VobTree&,std::string_view)) {
  auto cname = std::string(filename);
  auto i     = zenCache.find(cname);
  if(i!=zenCache.end())
    return i->second.get();

  std::vector<std::shared_ptr<asset::VirtualObject>> bundle;
  const bool overridden=readLayer(bundle,filename);
  if(!overridden) {
    try {
      WorldData::ZenFile base;
      if(loadBaseZen(cname,"world",base)) bundle=std::move(base.vobs);
      }
    catch(...) {
      Log::e("unable to load Zen-file: \"",cname,"\"");
      }
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


void Resources::probeMemory(std::string_view stage) {
  if(!LoadProbe::enabled()) return;
  std::lock_guard<std::recursive_mutex> lock(inst->sync);
  size_t models=0,npcModels=0,skeletons=0,animations=0;
  for(const auto& [name,mesh]:inst->aniMeshCache) {
    if(!mesh) continue;
    models+=mesh->probeCpuBytes();
    if(LoadProbe::Requests::npcModels.count(name)) npcModels+=mesh->probeCpuBytes();
    if(mesh->skeleton) skeletons+=mesh->skeleton->probeCpuBytes();
    }
  for(const auto& [name,animation]:inst->animCache)
    if(animation) animations+=animation->probeCpuBytes();
  for(const auto& [name,bytes]:{std::pair{"model_cpu",models},std::pair{"npc_model_cpu",npcModels},std::pair{"skeleton_cpu",skeletons},std::pair{"animation_cpu",animations}})
    Tempest::Log::i("[loadprobe-owner] stage=",stage," name=",name," bytes=",bytes);
  }

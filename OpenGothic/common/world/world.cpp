#include "world.h"

#include <functional>
#include <future>
#include <cctype>
#include <algorithm>
#include <cmath>
#include <chrono>

#include <Tempest/Log>
#include <Tempest/Painter>

#include "graphics/mesh/submesh/packedmesh.h"
#include "graphics/shaders.h"
#include "commandline.h"
#include "camera.h"
#include "graphics/visualfx.h"
#include "graphics/clothvisual.h"
#include "world/objects/globalfx.h"
#include "world/objects/npc.h"
#include "world/objects/item.h"
#include "world/objects/interactive.h"
#include "world/triggers/abstracttrigger.h"
#include "world/triggers/cscamera.h"
#include "game/bodyarena.h"
#include "game/globaleffects.h"
#include "game/serialize.h"
#include "game/physicssave.h"
#include "utils/string_frm.h"
#include "gothic.h"
#include "focus.h"
#include "resources.h"
#include "frameprofile.h"
#include "testbench.h"
#include "loadprobe.h"
#include "physics/physicsnoise.h"
#include "physics/cloth.h"
#include "physics/worldcloth.h"
#include "world/movablevobs.h"

namespace {

std::string_view traceObjectName(std::string_view what, bool arrow = false) {
  if(arrow) {
    if(what=="door")     return "стрела в двери";
    if(what=="table")    return "стрела в столе";
    if(what=="barrel")   return "стрела в бочке";
    if(what=="crate")    return "стрела в ящике";
    if(what=="cupboard") return "стрела в шкафу";
    if(what=="wall")     return "стрела в стене";
    return "стрела";
    }
  if(what=="table")    return "стол";
  if(what=="barrel")   return "бочка";
  if(what=="crate")    return "ящик";
  if(what=="cupboard") return "шкаф";
  return "предмет";
  }

std::string_view brokenObjectName(std::string_view what) {
  if(what=="crate")  return "обломки ящика";
  if(what=="barrel") return "обломки бочки";
  if(what=="fence")  return "сломанный забор";
  return "обломки предмета";
  }

int64_t traceTime(const World& world) {
  const auto now = world.time();
  return now.day()*24*60+now.hour()*60+now.minute();
  }

Tempest::Vec3 tracePoint(const Tempest::Matrix4x4& matrix, Tempest::Vec3 point) {
  matrix.project(point);
  return point;
  }

}

const char* materialTag(ItemMaterial src) {
  switch(src) {
    case ItemMaterial::MAT_WOOD:
      return "WO";
    case ItemMaterial::MAT_STONE:
      return "ST";
    case ItemMaterial::MAT_METAL:
      return "ME";
    case ItemMaterial::MAT_LEATHER:
      return "LE";
    case ItemMaterial::MAT_CLAY:
      return "CL";
    case ItemMaterial::MAT_GLAS:
      return "GL";
    case ItemMaterial::MAT_COUNT:
      return "UD";
    }
  return "UD";
  }

const char* materialTag(asset::MaterialGroup src) {
  switch(src) {
    case asset::MaterialGroup::UNDEFINED:
    case asset::MaterialGroup::NONE:
      return "UD";
    case asset::MaterialGroup::METAL:
      return "ME";
    case asset::MaterialGroup::STONE:
      return "ST";
    case asset::MaterialGroup::WOOD:
      return "WO";
    case asset::MaterialGroup::EARTH:
      return "EA";
    case asset::MaterialGroup::WATER:
      return "WA";
    case asset::MaterialGroup::SNOW:
      return "SA"; // sand?
    }
  return "UD";
  }

World::World(GameSession& game, std::string_view file, bool startup, std::function<void(int)> loadProgress)
  :wname(std::move(file)), game(game), wsound(game,*this), wobj(*this) {
  const auto loadStarted = std::chrono::steady_clock::now();
  Resources::configureTextures(Gothic::settingsGetS("GAME","texture_budget_mb"),Gothic::settingsGetS("GAME","texture_far_skip_mips"),
                               Gothic::settingsGetS("GAME","texture_target_mb"));
  if(LoadProbe::enabled())
    LoadProbe::mark("world_begin",-1,"world="+std::string(wname));
  try {
    WorldData::Data world;
    asset::WorldMesh worldMesh;
    Resources::loadWorld(world,worldMesh,wname,&chunks);
    if(LoadProbe::enabled())
      LoadProbe::mark("world_read_total",LoadProbe::msSince(loadStarted));
    residency = ChunkResidency::create(chunks);
    loadProgress(20);
    windRooms.reset(world.bsp.mode,std::move(world.rooms));
    auto worldCloth=WorldCloth::extract(worldMesh,&chunks);
    const bool streamLandscape=residency && !Resources::isHeadless() && !Gothic::inst().isBenchmarkMode() &&
        !CommandLine::inst().isRayQuery() && !Shaders::options().doRtScene && !Shaders::options().doSoftwareRT;
    if(residency && !Resources::isHeadless() && !streamLandscape) {
      if(Gothic::inst().isBenchmarkMode())
        Tempest::Log::i("Landscape streaming disabled: built-in benchmark requires the complete landscape");
      else
        Tempest::Log::i("Landscape streaming disabled: RT, GI or SWRT requires a whole-world BVH");
      }

    const bool probeSerial = LoadProbe::serial();
    const auto probeLaunch = probeSerial ? std::launch::deferred : std::launch::async;
    double probePhysicsMs = 0, probeViewMs = 0;
    auto wdynamicFut = std::async(probeLaunch, [&]() {
      Workers::setThreadName("Loading: BVH thread");
      const auto t = LoadProbe::now();
      auto ret = std::unique_ptr<DynamicWorld>(new DynamicWorld(this,worldMesh,&worldCloth.original));
      probePhysicsMs = LoadProbe::msSince(t);
      return ret;
      });
    std::future<std::unique_ptr<WorldView>> wviewFut;
    if(!Resources::isHeadless()) {
      wviewFut = std::async(probeLaunch, [&]() {
        Workers::setThreadName("Loading: PackedMesh thread");
        const auto t = LoadProbe::now();
        std::unique_ptr<WorldView> view;
        if(streamLandscape) {
          const auto map=PackedMesh::landscapeMaterialMap(worldMesh);
          {
          Resources::TextureLoadScope textureLoadScope(true);
          std::vector<bool> loaded(worldMesh.materials.size());
          // Decode initially used materials before publishing landscape chunks.
          for(auto id:worldMesh.polygons.material_indices) {
            id=map[id];
            if(!loaded[id]) {
              Resources::loadMaterial(worldMesh.materials[id],true);
              loaded[id]=true;
              }
            }
          }
          const auto& bounds=worldMesh.bbox;
          view=std::make_unique<WorldView>(std::pair{Tempest::Vec3(bounds.min.x,bounds.min.y,bounds.min.z),
                                                    Tempest::Vec3(bounds.max.x,bounds.max.y,bounds.max.z)},wname);
          }
        else {
          PackedMesh vmesh(worldMesh,PackedMesh::PK_VisualLnd);
          if(LoadProbe::enabled())
            LoadProbe::mark("landscape_pack",LoadProbe::msSince(t),
                            "vertices="+std::to_string(vmesh.vertices.size())+" indices="+std::to_string(vmesh.indices.size())+
                            " meshlet_bytes="+std::to_string(vmesh.vertices.size()*sizeof(vmesh.vertices[0])+vmesh.indices.size()*sizeof(vmesh.indices[0])));
          view=std::make_unique<WorldView>(vmesh,wname);
          }
        if(!worldCloth.original.polygons.material_indices.empty()) {
          PackedMesh originalCloth(worldCloth.original,PackedMesh::PK_VisualLnd);
          view->setOriginalClothMesh(originalCloth);
          view->setOriginalClothVisible(!Gothic::physicsFlags().cloth);
          }
        probeViewMs = LoadProbe::msSince(t);
        return view;
        });
      }

    loadProgress(30);

    {
      bsp.nodes             = std::move(world.bsp.nodes);
      bsp.sectors           = std::move(world.bsp.sectors);
      bsp.leaf_node_indices = std::move(world.bsp.leaf_node_indices);
      bsp.sectorsData.resize(bsp.sectors.size());
      world.bsp  = asset::BspTree();
    }
    loadProgress(50);

    auto probeT = LoadProbe::now();
    if(wviewFut.valid())
      wview = wviewFut.get();
    if(wview) {
      wview->setTextureResidency(residency.get());
      if(residency)
        wview->startVobStreaming(*residency);
      }
    if(LoadProbe::enabled() && !streamLandscape)
      LoadProbe::mark("landscape_view",probeViewMs,"wait_ms="+std::to_string(LoadProbe::msSince(probeT)));
    loadProgress(60);

    probeT = LoadProbe::now();
    wdynamic = wdynamicFut.get();
    wdynamic->setFurniturePartsEnabled(Gothic::physicsFlags().furnitureParts);
    if(LoadProbe::enabled()) {
      const auto& collision=wdynamic->staticCollisionLoadStats();
      LoadProbe::mark("physics_static",probePhysicsMs,"wait_ms="+std::to_string(LoadProbe::msSince(probeT))+
                      " collision="+(collision.cacheHit ? "cache" : "build")+
                      " key_ms="+std::to_string(collision.keyMs)+" read_ms="+std::to_string(collision.readMs)+
                      " build_ms="+std::to_string(collision.buildMs)+" write_ms="+std::to_string(collision.writeMs));
      }
    probeT = LoadProbe::now();
    wdynamic->cloth().setExposureTest([this](const Tempest::Vec3& point) { return !isIndoor(point); });
    wdynamic->setPhysicsNoiseEnabled(Gothic::physicsFlags().physicsNoise);
    wdynamic->setDestructionEnabled(Gothic::physicsFlags().destruction);
    wdynamic->setBuoyancyEnabled(Gothic::physicsFlags().buoyancy);
    wdynamic->setClothEnabled(Gothic::physicsFlags().cloth);
    wdynamic->setItemWakeEnabled(Gothic::physicsFlags().itemWake);
    wdynamic->setItemDeepestContactEnabled(Gothic::physicsFlags().itemDeepestContact);
    wdynamic->setPhysicsNoiseCallback([this](const PhysicsNoise& noise) { emitPhysicsNoise(noise); });
    for(auto& surface:worldCloth.surfaces)
      if(auto visual=ClothVisual::createSurface(*this,surface.mesh,surface.pins,surface.visual,surface.id,surface.stiffness,surface.linearDamping))
        clothSurfaces.push_back(std::move(visual));
    if(LoadProbe::enabled())
      LoadProbe::mark("cloth_surfaces",LoadProbe::msSince(probeT),"surfaces="+std::to_string(clothSurfaces.size()));
    probeT = LoadProbe::now();
    if(streamLandscape) {
      wview->startLandscapeStreaming(std::move(worldMesh),*residency,probeViewMs);
      // The visual consumer owns its selected file IDs; the stitched mesh is gone.
      std::vector<uint32_t>().swap(chunks.triangleChunks);
      std::vector<uint32_t>().swap(chunks.triangleSources);
      }
    else {
      worldMesh = {};
      chunks.source.reset();
      std::vector<uint32_t>().swap(chunks.triangleSources);
      }
    loadProgress(70);

    globFx.reset(new GlobalEffects(*this));
    wmatrix.reset(new WayMatrix(*this, *world.waynet));
    if(LoadProbe::enabled())
      LoadProbe::mark("waynet",LoadProbe::msSince(probeT));
    double probeVobsMs=0;
    {
    LoadProbe::Vobs::Stage probe;
    {
    Resources::TextureLoadScope textureLoadScope{bool(residency)};
    for(auto& vob:world.vobs)
      wobj.addRoot(vob,startup);
    }
    probeVobsMs=probe.stop();
    }
    if(LoadProbe::enabled())
      LoadProbe::mark("vobs",probeVobsMs,
                      "items="+std::to_string(wobj.itmCount())+" mobsi="+std::to_string(wobj.mobsiCount()));
    probeT = LoadProbe::now();
    wdynamic->finalizeMovableVobs();
    if(LoadProbe::enabled())
      LoadProbe::mark("movable_vobs",LoadProbe::msSince(probeT));

    probeT = LoadProbe::now();
    wobj.finalizePlacedItems();
    if(LoadProbe::enabled())
      LoadProbe::mark("placed_items",LoadProbe::msSince(probeT));

    probeT = LoadProbe::now();
    wmatrix->buildIndex();
    if(LoadProbe::enabled())
      LoadProbe::mark("waynet_index",LoadProbe::msSince(probeT));
    loadProgress(100);
    }
  catch(...) {
    Tempest::Log::e("unable to load landscape mesh");
    throw;
    }
  worldLoadMs = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-loadStarted).count();
  if(LoadProbe::enabled())
    LoadProbe::mark("world_end",worldLoadMs);
  }

World::~World() {
  }

void World::updateLandscape(const Camera& camera) {
  if(!residency || !npcPlayer) {
    if(textureLoadingPending && npcPlayer) {
      Resources::finishTextureLoading();
      textureLoadingPending=false;
      }
    Resources::updateTextures(nullptr);
    return;
    }
  if(wview)
    wview->prepareLandscapeWork();
  residency->update(npcPlayer->position(),camera.originLwc(),camera.zFar());
  if(wview)
    wview->finishLandscapeUpdate();
  if(textureLoadingPending) {
    Resources::finishTextureLoading();
    textureLoadingPending=false;
    }
  Resources::updateTextures(residency.get());
  }

void World::createPlayer(std::string_view cls) {
  size_t id = script().findSymbolIndex(cls);
  if(id==size_t(-1))
    return;
  std::string_view waypoint = wmatrix->startPoint().name;
  auto             npc      = std::make_unique<Npc>(*this, id, waypoint, NpcProcessPolicy::Player);
  npcPlayer = wobj.insertPlayer(std::move(npc), waypoint);
  game.script()->setInstanceNPC("HERO",*npcPlayer);
  }

void World::insertPlayer(std::unique_ptr<Npc> &&npc, std::string_view waypoint) {
  if(npc==nullptr)
    return;
  npcPlayer = wobj.insertPlayer(std::move(npc),waypoint);
  game.script()->setInstanceNPC("HERO",*npcPlayer);
  }

void World::setPlayer(Npc* npc) {
  if(npc==nullptr)
    return;
  npcPlayer->setProcessPolicy(NpcProcessPolicy::AiNormal);
  if(!npcPlayer->isDead()) {
    npcPlayer->resumeAiRoutine();
    }
  
  npc->setProcessPolicy(NpcProcessPolicy::Player);
  npc->clearState(true);
  npc->clearAiQueue();

  for(size_t i=0;i<PERC_Count;++i)
    npc->setPerceptionDisable(PercType(i));
  
  npcPlayer = npc;
  }

void World::postInit() {
  // NOTE: level inspector override player stats globaly
  // lvlInspector.reset(new Npc(*this,script().getSymbolIndex("PC_Levelinspektor"),""));

  // game.script()->inserNpc("Snapper",wmatrix->startPoint().name);
  }

void World::load(Serialize &fin) {
  spellFields.clear();
  stuckArrows.clear();
  nextArrowTraceId = 1;
  fin.setContext(this);
  fin.setEntry("worlds/",wname,"/world");

  uint32_t sz=0;
  fin.read(sz);
  std::string tag;
  for(size_t i=0;i<sz;++i) {
    int32_t guild=GIL_NONE;
    fin.read(tag,guild);
    if(auto p = portalAt(tag))
      p->guild = guild;
    }

  wobj.load(fin);
  npcPlayer = wobj.findHero();
  wdynamic->setTraceTime(traceTime(*this));
  wdynamic->loadMovableVobs(fin);
  loadStuckArrows(fin);
  }

void World::save(Serialize &fout) {
  fout.setContext(this);
  fout.setEntry("worlds/",wname,"/world");

  fout.write(uint32_t(bsp.sectorsData.size()));
  for(size_t i=0;i<bsp.sectorsData.size();++i) {
    fout.write(bsp.sectors[i].name,bsp.sectorsData[i].guild);
    }

  wobj.save(fout);
  wdynamic->saveMovableVobs(fout);
  saveStuckArrows(fout);
  wobj.savePlacedItems(fout);
  }

uint32_t World::npcId(const Npc *ptr) const {
  return wobj.npcId(ptr);
  }

Npc *World::npcById(uint32_t id) {
  if(id<wobj.npcCount())
    return &wobj.npc(id);
  return nullptr;
  }

uint32_t World::npcCount() const {
  return uint32_t(wobj.npcCount());
  }

Npc* World::npcForRagdoll(const Ragdoll* body) {
  if(body==nullptr)
    return nullptr;
  for(size_t i=0;i<wobj.npcCount();++i)
    if(wobj.npc(i).ragdollBody()==body)
      return &wobj.npc(i);
  return nullptr;
  }

bool World::stickArrow(MeshObjects::Mesh&& arrow, const Tempest::Matrix4x4& worldMatrix,
                      std::string_view visual, std::shared_ptr<const DynamicWorld::MovableVobPose> movable,
                      const Npc* culprit, const Tempest::Vec3* sightPoint, std::string_view surface) {
  if(arrow.isEmpty() || !Gothic::physicsFlags().arrowSticking || (movable!=nullptr && movable->broken))
    return false;
  if(stuckArrows.size()>=512)
    stuckArrows.erase(stuckArrows.begin());
  arrow.setObjMatrix(worldMatrix);
  StuckArrow pin;
  pin.mesh = std::move(arrow);
  pin.visual = std::string(visual);
  pin.local = worldMatrix;
  pin.traceId = nextArrowTraceId++;
  pin.appearedAt = traceTime(*this);
  pin.surface = std::string(surface);
  pin.sightPoint = sightPoint!=nullptr ? *sightPoint : Tempest::Vec3(worldMatrix.at(3,0),worldMatrix.at(3,1),worldMatrix.at(3,2));
  if(culprit!=nullptr) {
    if(culprit->isPlayer())
      pin.culprit = "PLAYER";
    else if(auto* symbol = script().findSymbol(culprit->instanceSymbol()))
      pin.culprit = symbol->name();
    }
  if(movable!=nullptr) {
    pin.local = movable->transform;
    pin.local.inverse();
    pin.local.project(pin.sightPoint);
    pin.local.mul(worldMatrix);
    pin.movable = movable;
    pin.attached = true;
    if(const auto* rule = MovableVobs::classifyVisual(movable->visual))
      pin.surface = rule->className;
    }
  stuckArrows.push_back(std::move(pin));
  return true;
  }

bool World::stuckArrowTransform(size_t index, Tempest::Matrix4x4& out) const {
  if(index>=stuckArrows.size() || stuckArrows[index].mesh.isEmpty())
    return false;
  out = stuckArrows[index].mesh.node(0).position();
  return true;
  }

bool World::stuckArrowAttachment(size_t index, uint32_t& zenId, Tempest::Matrix4x4& local) const {
  if(index>=stuckArrows.size())
    return false;
  const auto& pin = stuckArrows[index];
  auto pose = pin.movable.lock();
  if(!pin.attached || pose==nullptr || pose->broken)
    return false;
  zenId = pose->zenId;
  local = pin.local;
  return true;
  }

std::vector<World::PhysicsTrace> World::physicsTraces() const {
  std::vector<PhysicsTrace> result;
  if(Gothic::physicsFlags().arrowSticking) {
    for(const auto& pin:stuckArrows) {
      auto pose = pin.movable.lock();
      if(pin.mesh.isEmpty() || (pin.attached && pose==nullptr) ||
         (!pin.attached && (pin.surface=="ground" || pin.surface=="unknown")))
        continue;
      PhysicsTrace trace;
      trace.id = wname+":arrow:"+std::to_string(pin.traceId);
      trace.kind = "arrow";
      trace.name = traceObjectName(pin.surface,true);
      trace.position = pin.attached ? tracePoint(pose->transform,pin.sightPoint) : pin.sightPoint;
      trace.sightPoints.fill(trace.position);
      trace.appearedAt = pin.appearedAt;
      trace.culprit = pin.culprit;
      result.push_back(std::move(trace));
      }
    }
  for(const auto& item:wdynamic->movableVobSnapshot()) {
    if((!item.broken && (!item.enabled || !item.trace)) || item.zenId==uint32_t(-1))
      continue;
    PhysicsTrace trace;
    trace.id = wname+":vob:"+std::to_string(item.zenId);
    trace.kind = item.broken ? "broken" : item.tilted ? "tilted" : "moved";
    trace.name = item.broken ? brokenObjectName(item.className) : traceObjectName(item.className);
    const auto center = (item.localBounds[0]+item.localBounds[1])*.5f;
    trace.position = tracePoint(item.transform,center);
    // Broken parents retain their last intact pose: the trace describes where
    // the prop stood, independently of where its surviving pieces roll.
    // Surface samples stay just outside the mesh: a ray to the object's origin
    // would often end inside its own collider and hide a perfectly visible table.
    for(size_t axis=0;axis<3;++axis) {
      auto low = center, high = center;
      if(axis==0) { low.x = item.localBounds[0].x-2.f; high.x = item.localBounds[1].x+2.f; }
      if(axis==1) { low.y = item.localBounds[0].y-2.f; high.y = item.localBounds[1].y+2.f; }
      if(axis==2) { low.z = item.localBounds[0].z-2.f; high.z = item.localBounds[1].z+2.f; }
      trace.sightPoints[axis*2] = tracePoint(item.transform,low);
      trace.sightPoints[axis*2+1] = tracePoint(item.transform,high);
      }
    trace.appearedAt = item.traceTime;
    trace.culprit = item.traceCulprit;
    result.push_back(std::move(trace));
    }
  return result;
  }

void World::updateStuckArrows() {
  for(auto i=stuckArrows.begin();i!=stuckArrows.end();) {
    if(!i->attached) {
      ++i;
      continue;
      }
    auto pose = i->movable.lock();
    if(pose==nullptr || pose->broken) {
      i = stuckArrows.erase(i);
      continue;
      }
    auto matrix = pose->transform;
    matrix.mul(i->local);
    i->mesh.setObjMatrix(matrix);
    ++i;
    }
  }

void World::saveStuckArrows(Serialize& fout) const {
  if(!Gothic::physicsFlags().arrowSticking || (stuckArrows.empty() && nextArrowTraceId==1))
    return;
  std::vector<const StuckArrow*> pins;
  for(const auto& pin:stuckArrows) {
    auto pose = pin.movable.lock();
    if(pin.visual.empty() || (pin.attached && (pose==nullptr || pose->broken || pose->zenId==uint32_t(-1))))
      continue;
    pins.push_back(&pin);
    }
  fout.setEntry("worlds/",wname,"/stuck-arrows");
  fout.write(uint32_t(0x31524157),uint16_t(2),wname,uint32_t(pins.size()),nextArrowTraceId);
  for(const auto* pin:pins) {
    fout.write(pin->visual,pin->local,pin->attached);
    if(auto pose = pin->movable.lock(); pin->attached && pose!=nullptr)
      fout.write(pose->zenId,pose->visual,pose->origin);
    fout.write(pin->traceId,uint64_t(pin->appearedAt+1),pin->culprit,pin->surface,pin->sightPoint);
    }
  }

void World::loadStuckArrows(Serialize& fin) {
  if(!Gothic::physicsFlags().arrowSticking || !fin.setEntry("worlds/",wname,"/stuck-arrows"))
    return;
  try {
    uint32_t magic=0,count=0;
    uint16_t schema=0;
    std::string worldName;
    fin.read(magic,schema,worldName,count);
    if(magic!=0x31524157 || (schema!=1 && schema!=2) || worldName!=wname || count>512)
      return;
    if(schema>=2) {
      uint64_t nextId=1;
      fin.read(nextId);
      if(nextId==0 || nextId==UINT64_MAX)
        return;
      nextArrowTraceId = nextId;
      }
    std::vector<StuckArrow> pins;
    for(uint32_t i=0;i<count;++i) {
      StuckArrow pin;
      fin.read(pin.visual,pin.local,pin.attached);
      pin.traceId = uint64_t(i)+1;
      pin.sightPoint = Tempest::Vec3(pin.local.at(3,0),pin.local.at(3,1),pin.local.at(3,2));
      bool matches=true;
      if(pin.attached) {
        uint32_t id=uint32_t(-1);
        std::string visual;
        Tempest::Matrix4x4 origin;
        fin.read(id,visual,origin);
        auto pose = wdynamic->movableVobPose(id);
        matches = pose!=nullptr && pose->visual==visual && PhysicsSave::sameTransform(pose->origin,origin);
        pin.movable = pose;
        if(const auto* rule = MovableVobs::classifyVisual(visual))
          pin.surface = rule->className;
        }
      if(schema>=2) {
        uint64_t appeared=0;
        fin.read(pin.traceId,appeared,pin.culprit,pin.surface,pin.sightPoint);
        pin.appearedAt = appeared>0 && appeared<=uint64_t(INT64_MAX) ? int64_t(appeared-1) : -1;
        }
      if(!matches || pin.visual.empty() || pin.visual.size()>512 || !PhysicsSave::validTransform(pin.local) ||
         pin.traceId==0 || pin.traceId==UINT64_MAX || pin.culprit.size()>512 || pin.surface.size()>64 ||
         !std::isfinite(pin.sightPoint.x) || !std::isfinite(pin.sightPoint.y) || !std::isfinite(pin.sightPoint.z))
        continue;
      pin.mesh = addView(pin.visual);
      if(pin.mesh.isEmpty())
        continue;
      pin.mesh.setObjMatrix(pin.local);
      nextArrowTraceId = std::max(nextArrowTraceId,pin.traceId+1);
      pins.push_back(std::move(pin));
      }
    stuckArrows = std::move(pins);
    updateStuckArrows();
    }
  catch(const std::exception& e) {
    Tempest::Log::e("world arrows load: ",e.what());
    }
  }

uint32_t World::mobsiId(const Interactive* ptr) const {
  return wobj.mobsiId(ptr);
  }

Interactive* World::mobsiById(uint32_t id) {
  if(id<wobj.mobsiCount())
    return &wobj.mobsi(id);
  return nullptr;
  }

uint32_t World::itmId(const void *ptr) const {
  return wobj.itmId(ptr);
  }

Item *World::itmById(uint32_t id) {
  if(id<wobj.itmCount())
    return &wobj.itm(id);
  return nullptr;
  }

void World::runEffect(Effect&& e) {
  wobj.runEffect(std::move(e));
  }

void World::stopEffect(const VisualFx& root) {
  auto* vfx = &root;
  while(vfx!=nullptr) {
    globFx->stopEffect(*vfx);
    vfx = vfx->emFXCreate;
    }
  wobj.stopEffect(root);
  }

GlobalFx World::addGlobalEffect(std::string_view what, uint64_t len, const std::string* argv, size_t argc) {
  return globFx->startEffect(what,len,argv,argc);
  }

MeshObjects::Mesh World::addView(std::string_view visual) const {
  return addView(visual,0,0,0);
  }

MeshObjects::Mesh World::addView(std::string_view visual, int32_t headTex, int32_t teetTex, int32_t bodyColor) const {
  if(Resources::isHeadless()) {
    if(auto mesh=Resources::loadMesh(visual))
      return MeshObjects::Mesh(cpuObjects,*mesh,headTex,teetTex,bodyColor,false);
    return MeshObjects::Mesh();
    }
  return view()->addView(visual,headTex,teetTex,bodyColor);
  }

MeshObjects::Mesh World::addView(const zenkit::IItem& itm) {
  if(Resources::isHeadless())
    return addView(itm.visual,itm.material,0,itm.material);
  return view()->addView(itm.visual,itm.material,0,itm.material);
  }

MeshObjects::Mesh World::addView(const ProtoMesh* visual) {
  if(Resources::isHeadless())
    return visual!=nullptr ? MeshObjects::Mesh(cpuObjects,*visual,0,0,0,false) : MeshObjects::Mesh();
  return view()->addView(visual);
  }

MeshObjects::Mesh World::addAtachView(const ProtoMesh::Attach& visual, const int32_t version) {
  if(Resources::isHeadless())
    return MeshObjects::Mesh(cpuObjects,visual,version,false);
  return view()->addAtachView(visual,version);
  }

MeshObjects::Mesh World::addStaticView(const ProtoMesh* visual, bool staticDraw) {
  if(Resources::isHeadless())
    return visual!=nullptr ? MeshObjects::Mesh(cpuObjects,*visual,0,0,0,staticDraw) : MeshObjects::Mesh();
  return view()->addStaticView(visual,staticDraw);
  }

MeshObjects::Mesh World::addStaticView(std::string_view visual) {
  if(Resources::isHeadless())
    return addStaticView(Resources::loadMesh(visual),true);
  return view()->addStaticView(visual);
  }

MeshObjects::Mesh World::addDecalView(const asset::VisualDecal& decal) {
  if(Resources::isHeadless())
    return MeshObjects::Mesh();
  return view()->addDecalView(decal);
  }

LightGroup::Light World::addLight(const asset::VLight& vob) {
  if(Resources::isHeadless())
    return LightGroup::Light();
  return view()->addLight(vob, tickCount());
  }

LightGroup::Light World::addLight(std::string_view preset) {
  if(Resources::isHeadless())
    return LightGroup::Light();
  return view()->addLight(preset, tickCount());
  }

void World::updateAnimation(uint64_t dt) {
  wobj.updateAnimation(dt);
  }

void World::resetPositionToTA() {
  wobj.resetPositionToTA();
  }

std::unique_ptr<Npc> World::takeHero() {
  return wobj.takeNpc(npcPlayer);
  }

Item* World::findItemByInstance(size_t instance, size_t n) {
  return wobj.findItemByInstance(instance,n);
  }

Npc *World::findNpcByInstance(size_t instance, size_t n) {
  return wobj.findNpcByInstance(instance,n);
  }

std::string_view World::roomAt(const Tempest::Vec3& p) {
  if(bsp.nodes.empty())
    return "";

  const auto* node=&bsp.nodes[0];

  while(true) {
    const auto v    = node->plane;
    float        sgn  = v.x*p.x + v.y*p.y + v.z*p.z - v.w;
    uint32_t     next = (sgn>0) ? uint32_t(node->front_index) : uint32_t(node->back_index);
    if(next>=bsp.nodes.size())
      break;

    node = &bsp.nodes[next];
    }

  if(node->bbox.min.x <= p.x && p.x <node->bbox.max.x &&
     node->bbox.min.y <= p.y && p.y <node->bbox.max.y &&
     node->bbox.min.z <= p.z && p.z <node->bbox.max.z) {
    return roomAt(*node);
    }

  return "";
  }

std::string_view World::roomAt(const asset::BspNode& node) {
  const std::string* ret=nullptr;
  size_t       count=0;
  auto         id = &node-bsp.nodes.data();(void)id;

  for(auto& i:bsp.sectors) {
    for(auto r:i.node_indices)
      if(r<bsp.leaf_node_indices.size()){
        size_t idx = bsp.leaf_node_indices[r];
        if(idx>=bsp.nodes.size())
          continue;
        if(&bsp.nodes[idx]==&node) {
          ret = &i.name;
          count++;
          }
        }
    }
  if(count==1) {
    // TODO: portals
    return *ret;
    }
  static std::string empty;
  return empty;
  }

bool World::isIndoor(const Tempest::Vec3& point) const {
  return windRooms.isIndoor(point);
  }

World::BspSector* World::portalAt(std::string_view tag) {
  if(tag.empty())
    return nullptr;

  for(size_t i=0;i<bsp.sectors.size();++i)
    if(bsp.sectors[i].name==tag)
      return &bsp.sectorsData[i];
  return nullptr;
  }

void World::scaleTime(uint64_t& dt) {
  globFx->scaleTime(dt);
  }

void World::tick(uint64_t dt) {
  wdynamic->setTraceTime(traceTime(*this));
  wdynamic->setFurniturePartsEnabled(Gothic::physicsFlags().furnitureParts);
  wdynamic->setMovableVobsEnabled(Gothic::physicsFlags().movableVobs);
  wdynamic->setPhysicsNoiseEnabled(Gothic::physicsFlags().physicsNoise);
  wdynamic->setDestructionEnabled(Gothic::physicsFlags().destruction);
  wdynamic->setBuoyancyEnabled(Gothic::physicsFlags().buoyancy);
  wdynamic->setClothEnabled(Gothic::physicsFlags().cloth);
  wdynamic->setItemWakeEnabled(Gothic::physicsFlags().itemWake);
  wdynamic->setItemDeepestContactEnabled(Gothic::physicsFlags().itemDeepestContact);
  if(!Gothic::physicsFlags().arrowSticking)
    stuckArrows.clear();
  static bool doTicks=true;
  if(!doTicks)
    return;
  tickSpellFields(dt);
  { FrameProfile::Scope timing(FrameProfile::Zone::WorldLogic); wobj.tick(dt,dt); }
  { FrameProfile::Scope timing(FrameProfile::Zone::Prepare); wobj.prepareRagdolls(); }
  if(const auto* pl=player())
    wdynamic->setFragmentObserver(pl->position());
  { FrameProfile::Scope timing(FrameProfile::Zone::Physics); wdynamic->tick(dt); }
  updateStuckArrows();
  { FrameProfile::Scope timing(FrameProfile::Zone::Sync); wobj.syncRagdolls(); }
  FrameProfile::Scope timing(FrameProfile::Zone::WorldMisc);
  if(auto pl = player(); pl!=nullptr && !BodyArena::frozenWorld())
    wsound.tick(*pl);
  globFx->tick(dt);
  }

uint64_t World::tickCount() const {
  return game.tickCount();
  }

void World::setDayTime(int32_t h, int32_t min) {
  gtime now     = game.time();
  auto  day     = now.day();
  gtime dayTime = now.timeInDay();
  gtime next    = gtime(h,min);

  if(dayTime<=next){
    game.setTime(gtime(day,h,min));
    } else {
    game.setTime(gtime(day+1,h,min));
    }
  wobj.resetPositionToTA();
  }

gtime World::time() const {
  return game.time();
  }

Focus World::validateFocus(const Focus &def) {
  Focus ret = def;
  ret.npc         = wobj.validateNpc(ret.npc);
  ret.interactive = wobj.validateInteractive(ret.interactive);
  ret.item        = wobj.validateItem(ret.item);

  return ret;
  }

Focus World::findFocus(const Npc &pl, const Focus& def) {
  auto  opt      = WorldObjects::NoFlg;
  auto  collAlgo = TARGET_COLLECT_FOCUS;
  auto  collType = TARGET_TYPE_ALL;
  auto& policy   = searchPolicy(pl,collAlgo,collType,opt);

  WorldObjects::SearchOpt optNpc {policy.npc_range1,  policy.npc_range2,  policy.npc_azi,  collAlgo, collType, opt};
  WorldObjects::SearchOpt optMob {policy.mob_range1,  policy.mob_range2,  policy.mob_azi,  collAlgo};
  WorldObjects::SearchOpt optItm {policy.item_range1, policy.item_range2, policy.item_azi, collAlgo, collType};

  if(pl.weaponState()==WeaponState::NoWeapon) {
    // used only for dialogs it seems
    optNpc.rangeMax = std::max(optNpc.rangeMax, policy.npc_longrange);
    }

  auto n     = policy.npc_prio <0 ? nullptr : wobj.findNpcNear    (pl,def.npc,        optNpc);
  auto it    = policy.item_prio<0 ? nullptr : wobj.findItem       (pl,def.item,       optItm);
  auto inter = policy.mob_prio <0 ? nullptr : wobj.findInteractive(pl,def.interactive,optMob);
  auto ws = pl.weaponState();
  if(ws==WeaponState::Bow || ws==WeaponState::CBow) {
    optMob.flags = WorldObjects::SearchFlg(WorldObjects::FcOverride | WorldObjects::NoRay);
    inter = wobj.findInteractive(pl,def.interactive,optMob);
    }

  if(policy.npc_prio>=policy.item_prio &&
     policy.npc_prio>=policy.mob_prio) {
    if(n)
      return Focus(*n);
    if(policy.item_prio>=policy.mob_prio && it)
      return Focus(*it);
    return inter ? Focus(*inter) : Focus();
    }

  if(policy.mob_prio>=policy.item_prio &&
     policy.mob_prio>=policy.npc_prio) {
    if(inter)
      return Focus(*inter);
    if(policy.npc_prio>=policy.item_prio && n)
      return Focus(*n);
    return it ? Focus(*it) : Focus();
    }

  if(policy.item_prio>=policy.mob_prio &&
     policy.item_prio>=policy.npc_prio) {
    if(it)
      return Focus(*it);
    if(policy.npc_prio>=policy.mob_prio && n)
      return Focus(*n);
    return inter ? Focus(*inter) : Focus();
    }

  return Focus();
  }

Focus World::findFocus(const Focus &def) {
  if(npcPlayer==nullptr || npcPlayer->interactive()!=nullptr)
    return Focus();
  return findFocus(*npcPlayer,def);
  }

bool World::testFocusNpc(Npc* def) {
  auto  opt      = WorldObjects::NoFlg;
  auto  collAlgo = TARGET_COLLECT_FOCUS;
  auto  collType = TARGET_TYPE_ALL;
  auto& policy   = searchPolicy(*npcPlayer,collAlgo,collType,opt);

  WorldObjects::SearchOpt optNpc{policy.npc_range1, policy.npc_range2, policy.npc_azi, collAlgo, collType, opt};
  return wobj.testFocusNpc(*npcPlayer,def,optNpc);
  }

Interactive *World::availableMob(const Npc &pl, std::string_view name) {
  return wobj.availableMob(pl,name);
  }

Interactive* World::findInteractive(const Npc& pl) {
  WorldObjects::SearchOpt optMvMob;
  optMvMob.rangeMax = 100;
  optMvMob.flags    = WorldObjects::SearchFlg::NoAngle;

  return wobj.findInteractive(pl,nullptr,optMvMob);
  }

void World::triggerEvent(const TriggerEvent &e) {
  wobj.triggerEvent(e);
  }

void World::execTriggerEvent(const TriggerEvent& e) {
  if(e.timeBarrier > this->tickCount()) {
    triggerEvent(std::move(e));
    return;
    }

  bool emitted = wobj.execTriggerEvent(e);
  if(!emitted) {
    emitted = wsound.execTriggerEvent(e);
    }

  if(!emitted) {
    if(e.target=="EVT_LEFT_ROOM_01_TRAP_MOVER_FOR_DMG_MASTER" ||
       e.target=="EVT_LEFT_UP_01_TOGGLE_TRIGGER_01" ||
       e.target=="EVT_RIGHT_ROOM_01_SPAWN_ROT_02_SOUND" ||
       e.target=="NULL")
      return; // known problem on dragonisland.zen, skip for now
    Tempest::Log::d("unable to process trigger: \"",e.target,"\"");
    }
  }

void World::enableDefTrigger(AbstractTrigger& t) {
  wobj.enableDefTrigger(t);
  }

void World::enableTicks(AbstractTrigger& t) {
  wobj.enableTicks(t);
  }

void World::disableTicks(AbstractTrigger& t) {
  wobj.disableTicks(t);
  }

void World::setCurrentCs(CsCamera* cs) {
  wobj.setCurrentCs(cs);
  }

CsCamera* World::currentCs() const {
  return wobj.currentCs();
  }

bool World::isCutsceneLock() const {
  if(auto cs = wobj.currentCs())
    return !cs->isPlayerMovable();
  return false;
  }

void World::enableCollizionZone(CollisionZone& z) {
  wobj.enableCollizionZone(z);
  }

void World::disableCollizionZone(CollisionZone& z) {
  wobj.disableCollizionZone(z);
  }

void World::triggerChangeWorld(std::string_view world, std::string_view wayPoint) {
  game.changeWorld(world,wayPoint);
  }

void World::setMobRoutine(gtime time, std::string_view scheme, int32_t state) {
  wobj.setMobRoutine(time,scheme,state);
  }

void World::marchInteractives(DbgPainter &p) const {
  wobj.marchInteractives(p);
  }

void World::marchPoints(DbgPainter& p) const {
  wmatrix->marchPoints(p);
  }

void World::marchCsCameras(DbgPainter& p) const {
  wobj.marchCsCameras(p);
  }

void World::drawMeleeShadow(DbgPainter& p) const {
  wobj.drawMeleeShadow(p);
  }

void World::drawVobBoxNpcNear(DbgPainter& p) const {
  wobj.drawVobBoxNpcNear(p);
  }

AiOuputPipe *World::openDlgOuput(Npc &player, Npc &npc) {
  return game.openDlgOuput(player,npc);
  }

void World::aiOutputSound(Npc &player, std::string_view msg) {
  wsound.aiOutput(player.position(),msg);
  }

bool World::isInDialog() const {
  return game.isInDialog();
  }

bool World::isTargeted(Npc& npc) {
  return wobj.isTargeted(npc);
  }

Npc *World::addNpc(std::string_view name, std::string_view at) {
  size_t id = script().findSymbolIndex(name);
  if(id==size_t(-1))
    return nullptr;
  return wobj.addNpc(id,at);
  }

Npc *World::addNpc(size_t npcInstance, std::string_view at) {
  return wobj.addNpc(npcInstance,at);
  }

Npc* World::addNpc(size_t npcInstance, const Tempest::Vec3& at) {
  return wobj.addNpc(npcInstance,at);
  }

void World::removeNpc(Npc& npc) {
  return wobj.removeNpc(npc);
  }

Item *World::addItem(size_t itemInstance, std::string_view at) {
  return wobj.addItem(itemInstance,at);
  }

Item* World::addItem(const asset::VItem& vob) {
  return wobj.addItem(vob);
  }

Item* World::addItem(size_t itemInstance, const Tempest::Vec3& pos) {
  return wobj.addItem(itemInstance, pos);
  }

Item* World::addItemDyn(size_t itemInstance, const Tempest::Matrix4x4& pos, size_t owner) {
  return wobj.addItemDyn(itemInstance, pos, owner);
  }

std::unique_ptr<Item> World::takeItem(Item &it) {
  return wobj.takeItem(it);
  }

void World::removeItem(Item& it) {
  wobj.removeItem(it);
  }

size_t World::hasItems(std::string_view tag, size_t itemCls) {
  return wobj.hasItems(tag,itemCls);
  }

void World::applySpellImpact(int32_t spellId, const Tempest::Vec3& point, Npc* caster, Npc* target) {
  using namespace Tempest;
  if(!Gothic::physicsFlags().spellPhysics)
    return;
  const auto kind = script().physicalSpell(spellId);
  const bool wind = kind==GameScript::PhysicalSpell::WindFist;
  if(!wind && kind!=GameScript::PhysicalSpell::Fireball)
    return;
  if(wind && caster==nullptr)
    return;
  const Vec3 center = wind ? caster->collosionCenter() : point;
  const float radius = wind ? 1000.f : 400.f;
  auto forward = point-center;
  if(forward.length()<1.e-3f && caster!=nullptr)
    forward = Vec3(std::cos(caster->rotationRad()),0,std::sin(caster->rotationRad()));
  if(forward.length()>1.e-3f)
    forward = Vec3::normalize(forward);
  auto clearPath = [&](const Vec3& to) {
    const auto delta = to-center;
    if(delta.length()<4.f)
      return true;
    const auto hit = physic()->ray(center+Vec3::normalize(delta)*2.f,to);
    return !hit.hasCol || (hit.v-to).length()<8.f;
    };
  auto impulseAt = [&](const Vec3& to, bool testOcclusion=true) {
    const auto delta = to-center;
    const float distance = delta.length();
    if(distance>=radius || (testOcclusion && !clearPath(to)))
      return Vec3();
    const auto radial = distance>1.e-3f ? delta/distance : Vec3(0,1,0);
    if(wind && Vec3::dotProduct(radial,forward)<0.819152f) // 35 degree cone
      return Vec3();
    const float falloff = 1.f-distance/radius;
    auto direction = wind ? forward+Vec3(0,0.35f,0) : radial+Vec3(0,0.35f,0);
    return Vec3::normalize(direction)*((wind ? 450.f : 300.f)*falloff);
    };
  size_t npcHits = 0, itemHits = 0;
  detectNpc(center,radius,[&](Npc& npc) {
    if(&npc==caster || (wind && &npc!=target))
      return;
    const auto impulse = impulseAt(npc.collosionCenter());
    // A heavy creature resists this finite wind impulse. The threshold is a
    // delta velocity, so object scale and species mass use the same rule.
    if(impulse.quadLength()<=0.f || (wind && impulse.length()/npc.physicsMass()<1.f))
      return;
    if(!npc.addSpellImpulse(impulse,true,caster))
      return;
    ++npcHits;
    if(TestBench::isActive()) {
      const auto who = TestBench::who(&npc);
      char buf[256];
      std::snprintf(buf,sizeof(buf),"spell physics %s %.*s mass %.9g impulse %.9g %.9g %.9g",
                    wind ? "wind" : "fireball",int(who.size()),who.data(),double(npc.physicsMass()),
                    double(impulse.x),double(impulse.y),double(impulse.z));
      TestBench::note(buf);
      }
    });
  detectItem(center,radius,[&](Item& item) {
    const auto impulse = impulseAt(item.midPosition());
    if(impulse.quadLength()>0.f && item.addSpellImpulse(impulse,caster))
      ++itemHits;
    });
  const auto vobHits = physic()->applyMovableVobImpulse(center,radius,[&](const Vec3& to, float mass) {
    auto impulse = impulseAt(to,false);
    // Small vessels should scatter through the room. The same finite blast
    // impulse as furniture would launch a light cup at kilometres per second.
    const float limit = mass*10.f; // at most 10 m/s of added velocity
    const float length = impulse.length();
    if(length>limit)
      impulse *= limit/length;
    return impulse;
    },caster);
  if(TestBench::isActive()) {
    char buf[256];
    std::snprintf(buf,sizeof(buf),"spell physics impact id %d at %.9g %.9g %.9g npc %zu items %zu vobs %zu",
                  spellId,double(point.x),double(point.y),double(point.z),npcHits,itemHits,vobHits);
    TestBench::note(buf);
    }
  }

void World::startSpellField(int32_t spellId, const Tempest::Vec3& point, Npc* caster, Npc* target) {
  if(!Gothic::physicsFlags().spellPhysics ||
     script().physicalSpell(spellId)!=GameScript::PhysicalSpell::Whirlwind)
    return;
  int seconds = 10;
  if(auto* duration = script().findSymbol("SPL_TIME_WHIRLWIND"))
    seconds = std::clamp(duration->get_int(),1,60);
  if(spellFields.size()>=16)
    spellFields.erase(spellFields.begin());
  spellFields.push_back({point,caster,target,tickCount()+uint64_t(seconds)*1000,spellId});
  if(TestBench::isActive()) {
    char buf[192];
    std::snprintf(buf,sizeof(buf),"spell physics field start id %d at %.9g %.9g %.9g duration %d",
                  spellId,double(point.x),double(point.y),double(point.z),seconds);
    TestBench::note(buf);
    }
  }

void World::tickSpellFields(uint64_t dt) {
  using namespace Tempest;
  if(!Gothic::physicsFlags().spellPhysics) {
    spellFields.clear();
    return;
    }
  for(auto i=spellFields.begin();i!=spellFields.end();) {
    if(tickCount()<i->until) {
      ++i;
      continue;
      }
    if(TestBench::isActive()) {
      char buf[192];
      std::snprintf(buf,sizeof(buf),"spell physics field end id %d at %.9g %.9g %.9g",i->spellId,
                    double(i->center.x),double(i->center.y),double(i->center.z));
      TestBench::note(buf);
      }
    i = spellFields.erase(i);
    }
  const float seconds = float(std::min<uint64_t>(dt,50))*0.001f;
  if(seconds<=0.f)
    return;
  for(const auto& field:spellFields) {
    auto* caster = wobj.validateNpc(field.caster);
    auto impulseAt = [&](const Vec3& position) {
      const auto r = position-field.center;
      const float distance = Vec3(r.x,0,r.z).length();
      if(distance>=350.f || r.y>400.f || r.y< -100.f)
        return Vec3();
      const auto from = field.center+Vec3(0,75,0);
      if(physic()->ray(from,position+Vec3(0,30,0)).hasCol)
        return Vec3();
      const float falloff = 1.f-distance/350.f;
      // World-space force in newtons: lift loses strength above the vortex,
      // while the horizontal component turns inward and around its centre.
      const Vec3 force(-r.z*4.f-r.x*2.f+80.f,
                       std::clamp(1800.f-r.y*6.f,-750.f,1800.f),
                       r.x*4.f-r.z*2.f);
      return force*(seconds*falloff);
      };
    detectNpc(field.center,550.f,[&](Npc& npc) {
      if(&npc==field.target && &npc!=field.caster)
        npc.addSpellImpulse(impulseAt(npc.position()),false,caster);
      });
    detectItem(field.center,550.f,[&](Item& item) {
      const auto impulse = impulseAt(item.position());
      if(impulse.quadLength()>0.f)
        item.addSpellImpulse(impulse,caster);
      });
    }
  }

Bullet& World::shootSpell(const Item &itm, const Npc &npc, const Npc *target) {
  Tempest::Vec3   dir     = {1.f,0.f,0.f};
  auto            pos     = npc.collosionCenter();
  const VisualFx* vfx     = script().spellVfx(itm.spellId());
  float           tgRange = vfx==nullptr ? 0 : vfx->emTrjTargetRange;

  if(target!=nullptr) {
    auto tgPos = target->collosionCenter();
    if(vfx!=nullptr && !vfx->emTrjOriginNode.empty()) {
      pos = npc.mapBone(vfx->emTrjOriginNode);
      }
    if(vfx!=nullptr && !vfx->emTrjTargetNode.empty()) {
      tgPos = target->mapBone(vfx->emTrjTargetNode);
      }
    dir = tgPos-pos;
    } else {
    float a = npc.rotationRad();
    dir.x = std::cos(a);
    dir.z = std::sin(a);
    pos  = npc.mapWeaponBone();
    }

  auto& b = wobj.shootBullet(itm, pos, dir, tgRange, DynamicWorld::spellSpeed);
  b.setTarget(target);
  return b;
  }

Tempest::Vec3 World::bulletVelocity(const Tempest::Vec3& from, const Tempest::Vec3& to) {
  Tempest::Vec3 dir = to - from;

  float lxz   = std::sqrt(dir.x*dir.x+0*0+dir.z*dir.z);
  float speed = DynamicWorld::bulletSpeed;
  float t     = lxz/speed;

  dir/=t;
  dir.y += 0.5f*DynamicWorld::gravity*t;
  return dir;
  }

Bullet& World::shootBullet(const Item &itm, const Npc &npc, const Npc *target, const Interactive* inter) {
  Tempest::Vec3 dir = {1.f,0.f,0.f};
  auto          pos = npc.mapWeaponBone();

  if(target!=nullptr) {
    dir = bulletVelocity(pos,target->collosionCenter());
    } else
  if(inter!=nullptr) {
    dir = bulletVelocity(pos,inter->position());
    } else {
    float a = npc.rotationRad();
    dir.x = std::cos(a);
    dir.z = std::sin(a);
    }

  auto& b = wobj.shootBullet(itm, pos, dir, 0, DynamicWorld::bulletSpeed);
  b.setTarget(target);
  return b;
  }

Bullet& World::shootArrow(const Item& itm, const Tempest::Vec3& pos, const Tempest::Vec3& dir, float speed) {
  auto& b = wobj.shootBullet(itm, pos, dir, 0, speed);
  b.setTarget(nullptr);
  return b;
  }

void World::sendPassivePerc(Npc &self, Npc &other, int32_t perc) {
  wobj.sendPassivePerc(self,other,nullptr,nullptr,perc);
  }

void World::sendPassivePerc(Npc& self, Npc& other, Npc& victim, int32_t perc, std::shared_ptr<HeroDeed> deed) {
  wobj.sendPassivePerc(self,other,&victim,nullptr,perc,std::move(deed));
  }

void World::sendPassivePerc(Npc& self, Npc& other, Npc& victim, Item& item, int32_t perc) {
  wobj.sendPassivePerc(self,other,&victim,&item,perc);
  }

void World::sendPassivePerc(Npc &self, Npc &other, Item& item, int32_t perc, std::shared_ptr<HeroDeed> deed) {
  wobj.sendPassivePerc(self,other,nullptr,&item,perc,std::move(deed));
  }

void World::sendImmediatePerc(Npc& self, Npc& other, Npc& victim, int32_t perc) {
  wobj.sendImmediatePerc(self,other,victim,nullptr,perc);
  }

void World::sendImmediatePerc(Npc& self, Npc& other, Npc& victim, Item& item, int32_t perc) {
  wobj.sendImmediatePerc(self,other,victim,&item,perc);
  }

Sound World::addWeaponHitEffect(Npc& src, const Bullet* srcArrow, Npc& reciver) {
  auto p0 = src.centerPosition();
  auto p1 = reciver.centerPosition();

  Tempest::Matrix4x4 pos;
  pos.identity();
  pos.translate((p0+p1)*0.5f);

  const char* armor = "FL";
  if(auto a = reciver.currentArmor()) {
    auto m = ItemMaterial(a->handle().material);
    // NOTE: in vanilla only those sfx are defined for armor
    if(m==ItemMaterial::MAT_METAL || m==ItemMaterial::MAT_WOOD)
      armor = materialTag(m);
    }

  if(srcArrow!=nullptr && !srcArrow->isSpell()) {
    auto m = ItemMaterial(srcArrow->itemMaterial());
    return addHitEffect(materialTag(m),armor,"IAM",pos);
    }

  if(auto w = src.inventory().activeWeapon()) {
    auto m = ItemMaterial(w->handle().material);
    return addHitEffect(materialTag(m),armor,"IAM",pos);
    }

  if(src.isMonster())
    return addHitEffect("JA",armor,"MAM",pos); else
    return addHitEffect("FI",armor,"MAM",pos);
  }

Sound World::addLandHitEffect(ItemMaterial src, asset::MaterialGroup reciver, const Tempest::Matrix4x4& pos) {
  // IHI - item hits item
  // IHL - Item hits Level
  return addHitEffect(materialTag(src),materialTag(reciver),"IHL",pos);
  }

void World::emitPhysicsNoise(const PhysicsNoise& noise) {
  // Gothic has no CS_IHL_CL_* instances. Its existing clay contact samples are
  // named for the reverse pair (wood/metal/stone hitting clay).
  const auto surface = noise.surface==asset::MaterialGroup::UNDEFINED || noise.surface==asset::MaterialGroup::NONE ?
                       "ST" : materialTag(noise.surface);
  const auto claySurface = noise.surface==asset::MaterialGroup::WOOD ? "WO" :
                           noise.surface==asset::MaterialGroup::METAL ? "ME" : "ST";
  const bool splash = noise.surface==asset::MaterialGroup::WATER;
  // Reuse the native splash sample already used by MoveAlgo::emitWaterSplash.
  std::string name = splash ? "CS_INTRO_WATERSPLASH.WAV" :
                     noise.material==MAT_CLAY ? std::string("CS_IHL_")+claySurface+"_CL.wav" :
                                               std::string("CS_IHL_")+materialTag(noise.material)+'_'+surface;
  auto sound = Sound(*this,Sound::T_Regular,name,noise.position,noise.radius,false);
  // Some Gothic installations reference clay samples without shipping them.
  // Reuse the existing vessel impact in that case, keeping the event's material.
  if(sound.isEmpty() && noise.material==MAT_CLAY && !splash) {
    name = std::string("CS_IHL_GL_")+surface;
    sound = Sound(*this,Sound::T_Regular,name,noise.position,noise.radius,false);
    }
  sound.setVolume(sound.volume()*noise.volume);
  sound.play();
  if(TestBench::isActive() || TestBench::isPhysicsNoiseDebug())
    TestBench::physicsNoise(noise,name,!sound.isEmpty());
  wobj.emitPhysicsNoise(noise);
  }

Sound World::addWeaponBlkEffect(ItemMaterial src, ItemMaterial reciver, const Tempest::Matrix4x4& pos) {
  // IAI - item attacks item
  return addHitEffect(materialTag(src),materialTag(reciver),"IAI",pos);
  }

Sound World::addHitEffect(std::string_view src, std::string_view dst, std::string_view scheme, const Tempest::Matrix4x4& pos) {
  Tempest::Vec3 pos3;
  pos.project(pos3);

  string_frm sound("CS_",scheme,'_',src,'_',dst);
  auto ret = Sound(*this,::Sound::T_Regular,sound,pos3,2500.f,false);

  string_frm buf("CPFX_",scheme,'_',src,'_',dst);
  if(Gothic::inst().loadParticleFx(buf,true)==nullptr) {
    if(dst=="ME")
      buf = string_frm("CPFX_",scheme,"_METAL");
    else if(dst=="WO")
      buf = string_frm("CPFX_",scheme,"_WOOD");
    else if(dst=="ST")
      buf = string_frm("CPFX_",scheme,"_STONE");
    else
      return ret;
    }
  if(Gothic::inst().loadParticleFx(buf,true)==nullptr)
    return ret;

  Effect e(PfxEmitter(*this,buf),"");
  e.setObjMatrix(pos);
  e.setActive(true);
  runEffect(std::move(e));
  return ret;
  }

bool World::isInSfxRange(const Tempest::Vec3& pos) const {
  return wsound.isInListenerRange(pos,WorldSound::talkRange);
  }

bool World::isInPfxRange(const Tempest::Vec3& p) const {
  if(Resources::isHeadless())
    return true; // Effects may carry gameplay collision/perception callbacks.
  return wview->isInPfxRange(p);
  }

void World::addDlgSound(std::string_view s, const Tempest::Vec3& pos, float range, uint64_t& timeLen,
                        std::shared_ptr<LipSync::Track>* voice) {
  auto sfx = wsound.addDlgSound(s,pos,range,timeLen,voice);
  sfx.play();
  }

void World::detectMover(const std::function<void(MoveTrigger&)>& f) {
  wobj.detectMover(f);
  }

void World::addTrigger(AbstractTrigger* trigger) {
  wobj.addTrigger(trigger);
  }

void World::addInteractive(Interactive* inter) {
  wobj.addInteractive(inter);
  }

void World::addStartPoint(const Tempest::Vec3& pos, const Tempest::Vec3& dir, std::string_view name) {
  wmatrix->addStartPoint(pos,dir,name);
  }

void World::addFreePoint(const Tempest::Vec3& pos, const Tempest::Vec3& dir, std::string_view name) {
  wmatrix->addFreePoint(pos,dir,name);
  }

void World::addSound(const asset::VirtualObject& vob) {
  if(vob.type==asset::VirtualObjectType::zCVobSound || vob.type==asset::VirtualObjectType::zCVobSoundDaytime) {
    wsound.addSound(reinterpret_cast<const asset::VSound&>(vob));
    }
  else if(vob.type==asset::VirtualObjectType::oCZoneMusic) {
    wsound.addZone(reinterpret_cast<const asset::VZoneMusic&>(vob));
    }
  else if(vob.type==asset::VirtualObjectType::oCZoneMusicDefault) {
    wsound.setDefaultZone(reinterpret_cast<const asset::VZoneMusicDefault&>(vob));
    }
  }

void World::invalidateVobIndex() {
  wobj.invalidateVobIndex();
  }

const zenkit::IFocus& World::searchPolicy(const Npc& pl, TargetCollect& collAlgo, TargetType& collType, WorldObjects::SearchFlg& opt) const {
  opt      = WorldObjects::NoFlg;
  collAlgo = TARGET_COLLECT_FOCUS;
  collType = TARGET_TYPE_ALL;

  switch(pl.weaponState()) {
    case WeaponState::Fist:
    case WeaponState::W1H:
    case WeaponState::W2H:
      opt = WorldObjects::NoDeath;
      return game.script()->focusMelee();
    case WeaponState::Bow:
    case WeaponState::CBow:
      opt = WorldObjects::SearchFlg(WorldObjects::NoDeath | WorldObjects::NoUnconscious);
      return game.script()->focusRange();
    case WeaponState::Mage:{
      if(auto weapon = pl.inventory().activeWeapon()) {
        int32_t id  = weapon->spellId();
        auto&   spl = script().spellDesc(id);
        collAlgo = TargetCollect(spl.target_collect_algo);
        collType = TargetType(spl.target_collect_type);
        }
      opt = WorldObjects::SearchFlg(WorldObjects::NoDeath | WorldObjects::NoUnconscious);
      return game.script()->focusMage();
      }
    case WeaponState::NoWeapon:
      return game.script()->focusNorm();
    }
  return game.script()->focusNorm();
  }

void World::triggerOnStart(bool firstTime) {
  wobj.triggerOnStart(firstTime);
  }

const WayPoint *World::findPoint(std::string_view name, bool inexact) const {
  return wmatrix->findPoint(name,inexact);
  }

const WayPoint* World::findWayPoint(std::string_view name) const {
  return wmatrix->findWayPoint(name);
  }

void World::forEachWayPoint(const std::function<void(const WayPoint&)>& f) const {
  wmatrix->forEachPoint(f);
  }

const WayPoint* World::findWayPoint(const Tempest::Vec3& pos) const {
  return wmatrix->findWayPoint(pos,[](const WayPoint&){ return true; });
  }

const WayPoint* World::findWayPoint(const Tempest::Vec3& pos, const std::function<bool(const WayPoint&)>& f) const {
  return wmatrix->findWayPoint(pos,f);
  }

const WayPoint* World::findWayPoint(const Tempest::Vec3& pos, std::string_view name) const {
  return wmatrix->findWayPoint(pos,[name](const WayPoint& wp) -> bool {
    if(wp.isLocked())
      return false;
    if(!wp.checkName(name,false))
      return false;
    return true;
    });
  }

const WayPoint* World::findFreePoint(const Npc &npc, std::string_view name) const {
  if(auto p = npc.currentWayPoint()){
    if(p->isFreePoint() && p->checkName(name)) {
      return p;
      }
    }

  const auto pos = npc.centerPosition();
  return wmatrix->findFreePoint(pos,name,[&npc](const WayPoint& wp) -> bool {
    if(wp.isLocked())
      return false;
    if(!npc.canRayHitPoint(Tempest::Vec3(wp.pos.x,wp.pos.y+10,wp.pos.z)))
      return false;
    return true;
    });
  }

const WayPoint* World::findFreePoint(const Tempest::Vec3& pos, std::string_view name) const {
  return wmatrix->findFreePoint(pos,name,[](const WayPoint& wp) -> bool {
    if(wp.isLocked())
      return false;
    return true;
    });
  }

const WayPoint* World::findNextFreePoint(const Npc &npc, std::string_view name) const {
  auto pos = npc.centerPosition();
  auto cur = npc.currentWayPoint();
  if(cur!=nullptr && !cur->checkName(name)) {
    cur = nullptr;
    }
  auto filter = [&](const WayPoint& wp) {
    if(wp.isLocked() || &wp==cur)
      return false;
    if(!npc.canRayHitPoint(Tempest::Vec3(wp.pos.x,wp.pos.y+10,wp.pos.z)))
      return false;
    return true;
    };
  auto wp  = wmatrix->findFreePoint(pos, name, filter);
  return wp;
  }

const WayPoint* World::findNextWayPoint(const Npc &npc) const {
  auto pos     = npc.centerPosition();
  auto nearest = npc.currentWayPoint();
  if(nearest==nullptr || nearest->isFreePoint()) {
    nearest = findWayPoint(pos);
    }
  auto filter  = [&](const WayPoint& wp) {
    if(!npc.canRayHitPoint(Tempest::Vec3(wp.pos.x,wp.pos.y+10,wp.pos.z)))
      return false;
    return nearest != &wp;
    };
  return nearest ? findWayPoint(pos, filter) : nullptr;
  }

const WayPoint *World::findNextPoint(const WayPoint &pos) const {
  return wmatrix->findNextPoint(pos.position());
  }

const WayPoint& World::startPoint() const {
  return wmatrix->startPoint();
  }

const WayPoint& World::deadPoint() const {
  return wmatrix->deadPoint();
  }

void World::detectNpcNear(std::function<void (Npc &)> f) {
  wobj.detectNpcNear(f);
  }

void World::detectNpc(const Tempest::Vec3& p, const float r, const std::function<void(Npc&)>& f) {
  wobj.detectNpc(p.x,p.y,p.z,r,f);
  }

void World::detectItem(const Tempest::Vec3& p, const float r, const std::function<void(Item&)>& f) {
  wobj.detectItem(p.x,p.y,p.z,r,f);
  }

WayPath World::wayTo(const WayPoint& begin, const WayPoint& end) const {
  const auto* first = &begin;
  return wmatrix->wayTo(&first,1,begin.position(),end);
  }

WayPath World::wayTo(const Npc &npc, const WayPoint &end) const {
  const auto npcPos = npc.centerPosition();

  auto begin = npc.currentWayPoint();
  if(begin==&end && MoveAlgo::isClose(npc,end)) {
    return WayPath();
    }
  if(begin==&end && !end.isConnected() && npc.canRayHitPoint(end.position()+Tempest::Vec3(0,10,0))) {
    WayPath ret;
    ret.add(end);
    return ret;
    }
  if(begin && begin->isConnected() && MoveAlgo::isClose(npc,*begin)) {
    return wmatrix->wayTo(&begin,1,npcPos,end);
    }
  auto near = wmatrix->findWayPoint(npcPos, [&npc](const WayPoint &wp) {
    if(!npc.canRayHitPoint(wp.pos))
      return false;
    return true;
    });

  if(near==nullptr) {
    // fallback to any closest point
    near = wmatrix->findWayPoint(npcPos, [](const WayPoint &wp) { return true; });
    }

  if(MoveAlgo::isClose(npc,*near) && near==&end)
    return WayPath();

  std::vector<const WayPoint*> wpoint;
  wpoint.push_back(near);
  for(auto& i:near->connections()) {
    auto p = i.point->position();
    if(npc.canRayHitPoint(Tempest::Vec3(p.x,p.y+10,p.z)))
      wpoint.push_back(i.point);
    }

  return wmatrix->wayTo(wpoint.data(),wpoint.size(),npcPos,end);
  }

GameScript& World::script() const {
  return *game.script();
  }

const VersionInfo& World::version() const {
  return game.version();
  }

void World::assignRoomToGuild(std::string_view r, int32_t guildId) {
  auto room = std::string(r);
  for(auto& i:room)
    i = char(std::toupper(i));

  if(auto rx=portalAt(room)){
    rx->guild=guildId;
    return;
    }

  Tempest::Log::d("room not found: ",room);
  }

int32_t World::guildOfRoom(const Tempest::Vec3& pos) {
  std::string_view tg = roomAt(pos);
  if(auto room=portalAt(tg)) {
    if(room->guild==GIL_PUBLIC) //FIXME: proper portal implementation
      return room->guild;
    }
  return GIL_NONE;
  }

int32_t World::guildOfRoom(std::string_view portalName) {
  size_t b = portalName.find(':');
  if(b==std::string::npos)
    return -1;
  b++;

  size_t e=portalName.find('_');
  if(e==std::string::npos || b>e)
    return -1;

  auto name = portalName.substr(b,e-b);

  for(size_t i=0;i<bsp.sectors.size();++i) {
    auto& s = bsp.sectors[i].name;
    if(s==name)
      return bsp.sectorsData[i].guild;
    }
  return GIL_NONE;
  }

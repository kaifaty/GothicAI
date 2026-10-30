#include "worldview.h"

#include <Tempest/Application>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <numeric>
#include <stdexcept>
#include <thread>

#include "graphics/mesh/submesh/packedmesh.h"
#include "game/gametime.h"
#include "camera.h"
#include "shaders.h"
#include "world/chunkresidency.h"
#include "loadprobe.h"
#include "utils/workers.h"

using namespace Tempest;

struct WorldView::LandscapeStreaming {
  struct Chunk {
    std::unique_ptr<Landscape> gpu;
    std::unique_ptr<PackedMesh> ready;
    std::exception_ptr error;
    bool failed=false;
    uint64_t generation=0;
    };
  WorldView& view;
  ChunkResidency& residency;
  std::shared_ptr<const ChunkMeshSource> source;
  std::vector<std::vector<uint32_t>> triangles;
  std::vector<uint32_t> materials;
  LandscapePages pages;
  std::vector<Chunk> chunks;
  std::mutex mutex;
  std::condition_variable wake;
  std::deque<std::pair<size_t,uint64_t>> jobs;
  size_t readyCount=0;
  bool packing=false;
  size_t releasedSourceBytes=0;
  bool stopping=false;
  bool firstUpdate=true;
  double preloadMs=0;
  double initialPackMs=0;
  double initialUploadMs=0;
  uint64_t subscription=0;
  std::thread worker;

  LandscapeStreaming(WorldView& view, asset::WorldMesh&& mesh, ChunkResidency& residency, double preloadMs)
    :view(view),residency(residency),source(residency.table().source),
     triangles(residency.table().entries.size()),materials(PackedMesh::landscapeMaterialMap(mesh)),
     chunks(triangles.size()),preloadMs(preloadMs) {
    if(!source)
      throw std::runtime_error("Landscape streaming requires a file-backed chunk source");
    const auto& table=residency.table();
    for(size_t i=0;i<triangles.size();++i)
      triangles[i].reserve(table.entries[i].triangleCount);
    for(size_t i=0;i<table.triangleChunks.size();++i)
      triangles.at(table.triangleChunks[i]).push_back(table.triangleSources.at(i));
    // Physics and cloth have consumed the stitched mesh. Repacking reads only
    // the selected chunk; no dense world geometry survives on the CPU.
    releasedSourceBytes=mesh.vertices.capacity()*sizeof(mesh.vertices[0])+
                        mesh.features.capacity()*sizeof(mesh.features[0])+
                        mesh.polygons.vertex_indices.capacity()*sizeof(mesh.polygons.vertex_indices[0])+
                        mesh.polygons.feature_indices.capacity()*sizeof(mesh.polygons.feature_indices[0])+
                        mesh.polygons.material_indices.capacity()*sizeof(mesh.polygons.material_indices[0])+
                        mesh.polygons.lightmap_indices.capacity()*sizeof(mesh.polygons.lightmap_indices[0])+
                        mesh.polygons.flags.capacity()*sizeof(mesh.polygons.flags[0]);
    mesh={};
    subscription=residency.subscribe([this](ChunkId id, ChunkResidency::State, ChunkResidency::State next) {
      const auto& entries=this->residency.table().entries;
      const auto at=std::find_if(entries.begin(),entries.end(),[&](const auto& e) { return e.id==id; });
      const size_t i=size_t(at-entries.begin());
      auto& chunk=chunks[i];
      if(next==ChunkResidency::State::Outside) {
        {
        std::lock_guard guard(mutex);
        ++chunk.generation;
        takeReady(chunk);
        }
        if(chunk.gpu) {
          chunk.gpu.reset();
          this->residency.recordRelease();
          }
        }
      else if(next==ChunkResidency::State::Near && !chunk.gpu) {
        // Near is a hard requirement, including teleports. Invalidate any
        // queued/in-flight Far result rather than waiting on the worker.
        std::unique_ptr<PackedMesh> packed;
        {
        std::lock_guard guard(mutex);
        if(chunk.failed)
          return;
        ++chunk.generation;
        packed=takeReady(chunk);
        }
        const auto t=ChunkResidency::Clock::now();
        try {
          if(!packed)
            packed=pack(i);
          }
        catch(...) {
          {
          std::lock_guard guard(mutex);
          chunk.failed=true;
          }
          reportFailure(i,std::current_exception());
          return;
          }
        if(firstUpdate)
          initialPackMs+=std::chrono::duration<double,std::milli>(ChunkResidency::Clock::now()-t).count();
        publish(i,*packed);
        }
      else if(next==ChunkResidency::State::Far && !chunk.gpu) {
        std::lock_guard guard(mutex);
        if(chunk.failed)
          return;
        jobs.emplace_back(i,++chunk.generation);
        wake.notify_one();
        }
      });
    worker=std::thread([this] { run(); });
    }

  ~LandscapeStreaming() {
    residency.unsubscribe(subscription);
    {
    std::lock_guard guard(mutex);
    stopping=true;
    }
    wake.notify_one();
    worker.join();
    }

  void run() {
    Workers::setThreadName("Landscape packing");
    for(;;) {
      size_t i;
      uint64_t generation;
      {
      std::unique_lock guard(mutex);
      // Publishing is limited to one package per frame; do not build an
      // unbounded second copy of the world while that queue catches up.
      wake.wait(guard,[&] { return stopping || (!jobs.empty() && readyCount==0); });
      if(stopping)
        return;
      std::tie(i,generation)=jobs.front();
      jobs.pop_front();
      if(chunks[i].generation!=generation || chunks[i].failed)
        continue;
      packing=true;
      }
      std::unique_ptr<PackedMesh> packed;
      std::exception_ptr error;
      try {
        packed=pack(i);
        }
      catch(...) {
        error=std::current_exception();
        }
      std::lock_guard guard(mutex);
      if(chunks[i].generation==generation) {
        chunks[i].ready=std::move(packed);
        chunks[i].error=error;
        chunks[i].failed=bool(error);
        readyCount+=chunks[i].ready!=nullptr;
        }
      else
        packed.reset();
      packing=false;
      }
    }

  // Called with mutex held. Waking the worker here bounds ready CPU packages
  // to one, without making the frame wait for any packing work.
  std::unique_ptr<PackedMesh> takeReady(Chunk& chunk) {
    auto packed=std::move(chunk.ready);
    if(packed) {
      --readyCount;
      wake.notify_one();
      }
    return packed;
    }

  bool ready() {
    std::lock_guard guard(mutex);
    if(packing || readyCount!=0 || !jobs.empty() || !residency.idle())
      return false;
    for(size_t i=0;i<chunks.size();++i)
      if(residency.desired(i)!=ChunkResidency::State::Outside && !chunks[i].gpu)
        return false;
    return true;
    }

  void reportFailure(size_t i, std::exception_ptr error) {
    const auto& chunk=residency.table().entries[i];
    try {
      std::rethrow_exception(error);
      }
    catch(const std::exception& e) {
      Log::e("Landscape chunk ",chunk.id.x,",",chunk.id.z," (",chunk.file,") packing failed: ",e.what());
      }
    catch(...) {
      Log::e("Landscape chunk ",chunk.id.x,",",chunk.id.z," (",chunk.file,") packing failed: unknown exception");
      }
    }

  std::unique_ptr<PackedMesh> pack(size_t i) const {
    auto chunk=source->read(i,triangles[i]);
    std::vector<uint32_t> selected(chunk.mesh.polygons.material_indices.size());
    std::iota(selected.begin(),selected.end(),uint32_t(0));
    return std::make_unique<PackedMesh>(chunk.mesh,selected,materials,chunk.vertexIds,chunk.featureIds);
    }

  void publish(size_t i, const PackedMesh& packed) {
    const auto t=ChunkResidency::Clock::now();
    chunks[i].gpu=std::make_unique<Landscape>(view.visuals,packed,pages);
    const auto ms=std::chrono::duration<double,std::milli>(ChunkResidency::Clock::now()-t).count();
    residency.recordPublication(ms,residency.desired(i)==ChunkResidency::State::Far);
    if(firstUpdate)
      initialUploadMs+=ms;
    }

  void prepare() {
    std::vector<std::pair<size_t,std::exception_ptr>> errors;
    {
    std::lock_guard guard(mutex);
    for(size_t i=0;i<chunks.size();++i)
      if(chunks[i].error) {
        errors.emplace_back(i,std::move(chunks[i].error));
        chunks[i].error=nullptr;
        }
    }
    for(auto& error:errors)
      reportFailure(error.first,error.second);
    // Exactly one Far publication is admitted each frame. Only complete CPU
    // packages enter the frame queue; it never waits for the packing worker.
    residency.enqueue([this](ChunkResidency::Clock::time_point deadline) {
      if(residency.stats().published!=publishedBefore || ChunkResidency::Clock::now()>=deadline)
        return true;
      for(size_t i=0;i<chunks.size();++i) {
        if(chunks[i].gpu || residency.desired(i)!=ChunkResidency::State::Far)
          continue;
        std::unique_ptr<PackedMesh> packed;
        {
        std::lock_guard guard(mutex);
        if(chunks[i].failed)
          continue;
        packed=takeReady(chunks[i]);
        }
        if(packed) {
          publish(i,*packed);
          break;
          }
        }
      return true;
      });
    publishedBefore=residency.stats().published;
    }
  uint64_t publishedBefore=0;

  void finish() {
    size_t pending=0;
    size_t readyBytes=0;
    {
    std::lock_guard guard(mutex);
    for(size_t i=0;i<chunks.size();++i) {
      pending+=residency.desired(i)!=ChunkResidency::State::Outside && !chunks[i].gpu && !chunks[i].failed;
      if(firstUpdate && chunks[i].ready) {
        const auto& packed=*chunks[i].ready;
        readyBytes+=packed.vertices.capacity()*sizeof(packed.vertices[0])+
                    packed.indices.capacity()*sizeof(packed.indices[0])+
                    packed.indices8.capacity()*sizeof(packed.indices8[0])+
                    packed.meshletBounds.capacity()*sizeof(packed.meshletBounds[0]);
        }
      }
    }
    residency.setPending(pending);
    const auto pageStats=pages.stats();
    residency.setLandscapePages(pageStats.occupiedBytes,pageStats.freeBytes,pageStats.retiredBytes,pageStats.pageCount);
    if(firstUpdate) {
      if(LoadProbe::enabled()) {
        size_t sourceBytes=source->memoryBytes()+materials.capacity()*sizeof(materials[0]);
        for(const auto& list:triangles)
          sourceBytes+=list.capacity()*sizeof(list[0]);
        LoadProbe::mark("landscape_pack",initialPackMs,"near_chunks="+std::to_string(residency.stats().near));
        LoadProbe::mark("landscape_view",preloadMs+initialPackMs+initialUploadMs,
                        "streaming=1 published="+std::to_string(residency.stats().published)+
                        " source_geometry_bytes="+std::to_string(sourceBytes)+
                        " ready_geometry_bytes="+std::to_string(readyBytes)+
                        " released_source_bytes="+std::to_string(releasedSourceBytes));
        }
      firstUpdate=false;
      }
    }
  };

WorldView::WorldView(const PackedMesh& wmesh, std::string_view skyPreset)
    : WorldView(wmesh.bbox(),skyPreset) {
  land=std::make_unique<Landscape>(visuals,wmesh);
  }

WorldView::WorldView(std::pair<Vec3,Vec3> bounds, std::string_view skyPreset)
    : aabb(bounds), gSky(skyPreset), gLights(sGlobal), visuals(sGlobal,bounds),
    objGroup(visuals), pfxGroup(*this,sGlobal,visuals) {
  pfxGroup.resetTicks();
  }

WorldView::~WorldView() {
  // cmd buffers must not be in use
  Resources::device().waitIdle();
  streaming.reset();
  }

void WorldView::startLandscapeStreaming(asset::WorldMesh&& mesh, ChunkResidency& residency, double preloadMs) {
  streaming=std::make_unique<LandscapeStreaming>(*this,std::move(mesh),residency,preloadMs);
  }

void WorldView::prepareLandscapeWork() {
  if(streaming)
    streaming->prepare();
  }

void WorldView::finishLandscapeUpdate() {
  if(streaming)
    streaming->finish();
  }

bool WorldView::landscapeReady() const {
  return !streaming || streaming->ready();
  }

const LightSource& WorldView::mainLight() const {
  return gSky.sunLight();
  }

const Tempest::Vec3& WorldView::ambientLight() const {
  return gSky.ambientLight();
  }

std::pair<Vec3, Vec3> WorldView::bbox() const {
  return aabb;
  }

bool WorldView::isInPfxRange(const Vec3& pos) const {
  return pfxGroup.isInPfxRange(pos);
  }

void WorldView::preFrameUpdate(const Camera& camera, uint64_t tickCount, uint8_t fId) {
  const auto ldir = gSky.sunLight().dir();
  Tempest::Matrix4x4 shadow   [Resources::ShadowLayers];
  Tempest::Matrix4x4 shadowLwc[Resources::ShadowLayers];
  for(size_t i=0; i<Resources::ShadowLayers; ++i) {
    shadow   [i] = camera.viewShadow(ldir,i);
    shadowLwc[i] = camera.viewShadowLwc(ldir,i);
    }

  sGlobal.setViewProject(camera.view(),camera.projective(),camera.zNear(),camera.zFar(),shadow);
  sGlobal.setViewLwc(camera.viewLwc(),camera.projective(),shadowLwc);
  sGlobal.setViewVsm(camera.viewShadowVsm(ldir), camera.viewShadowVsmLwc(ldir));
  sGlobal.originLwc = camera.originLwc();
  sGlobal.setUnderWater(camera.isInWater());
  sGlobal.setSky(gSky);
  sGlobal.setWorld(*this);

  pfxGroup.setViewerPos(camera.originLwc());
  pfxGroup.tick(tickCount);
  gLights.tick(tickCount);
  sGlobal.setTime(tickCount);
  sGlobal.commitUbo(fId);

  pfxGroup.preFrameUpdate(fId);
  visuals .preFrameUpdate();
  }

void WorldView::prepareGlobals(Tempest::Encoder<Tempest::CommandBuffer>& cmd, uint8_t fId) {
  sGlobal.prepareGlobals(cmd, fId);
  gLights.prepareGlobals(cmd, fId);
  visuals.prepareGlobals(cmd, fId);
  }

void WorldView::setGbuffer(const Texture2d& diffuse, const Texture2d& norm) {
  sGlobal.gbufDiffuse = &diffuse;
  sGlobal.gbufNormals = &norm;
  sGlobal.setResolution(uint32_t(diffuse.w()),uint32_t(diffuse.h()));
  }

void WorldView::setShadowMaps(const Tempest::Texture2d* sh[]) {
  const Texture2d* shadow[Resources::ShadowLayers] = {};
  for(size_t i=0; i<Resources::ShadowLayers; ++i)
    if(sh[i]==nullptr || sh[i]->isEmpty())
      shadow[i] = &Resources::fallbackBlack(); else
      shadow[i] = sh[i];
  sGlobal.setShadowMap(shadow);
  }

void WorldView::setVirtualShadowMap(bool enabled,
                                    const Tempest::ZBuffer&       pageData,
                                    const Tempest::StorageImage&  pageTbl,
                                    const Tempest::StorageImage&  pageHiZ,
                                    const Tempest::StorageBuffer& pageList)  {
  sGlobal.setVirtualShadowMap(enabled, pageData, pageTbl, pageHiZ, pageList);
  }

void WorldView::setHiZ(const Tempest::Texture2d& hiZ) {
  sGlobal.setHiZ(hiZ);
  }

void WorldView::setSceneImages(const Tempest::Texture2d& clr, const Tempest::Texture2d& depthAux, const Tempest::ZBuffer& depthNative) {
  sGlobal.sceneColor = &clr;
  sGlobal.sceneDepth = &depthAux;
  sGlobal.zbuffer    = &textureCast<const Texture2d&>(depthNative);
  }

void WorldView::setWindEnabled(bool enabled, uint64_t period) {
  sGlobal.zWindEnabled = enabled && period>0;
  sGlobal.windPeriod   = period;
  }

void WorldView::setClothWind(bool enabled, float scale) {
  sGlobal.legacyWind = !enabled;
  sGlobal.windScale = scale;
  }

void WorldView::setOriginalClothMesh(const PackedMesh& mesh) {
  originalCloth=std::make_unique<Landscape>(visuals,mesh);
  }

void WorldView::setOriginalClothVisible(bool visible) {
  if(originalCloth)
    originalCloth->setVisible(visible);
  }

void WorldView::dbgLights(DbgPainter& p) const {
  gLights.dbgLights(p);
  }

void WorldView::updateFrustrum(const Frustrum fr[]) {
  for(uint8_t i=0; i<SceneGlobals::V_Count; ++i)
    sGlobal.frustrum[i] = fr[i];
  }

void WorldView::visibilityPass(Tempest::Encoder<Tempest::CommandBuffer>& cmd, int pass) {
  cmd.setDebugMarker("Visibility");
  visuals.visibilityPass(cmd, pass);
  }

void WorldView::visibilityVsm(Tempest::Encoder<Tempest::CommandBuffer>& cmd) {
  visuals.visibilityVsm(cmd);
  }

void WorldView::drawHiZ(Tempest::Encoder<Tempest::CommandBuffer>& cmd) {
  visuals.drawHiZ(cmd);
  }

void WorldView::drawShadow(Tempest::Encoder<CommandBuffer>& cmd, uint8_t fId, uint8_t layer) {
  visuals.drawShadow(cmd,layer);
  pfxGroup.drawShadow(cmd,fId,layer);
  }

void WorldView::drawVsm(Tempest::Encoder<Tempest::CommandBuffer>& cmd) {
  visuals.drawVsm(cmd);
  }

void WorldView::drawGBuffer(Tempest::Encoder<CommandBuffer>& cmd, uint8_t fId) {
  visuals.drawGBuffer(cmd);
  pfxGroup.drawGBuffer(cmd, fId);
  }

void WorldView::drawWater(Tempest::Encoder<Tempest::CommandBuffer>& cmd) {
  visuals.drawWater(cmd);
  }

void WorldView::drawTranslucent(Tempest::Encoder<CommandBuffer>& cmd, uint8_t fId) {
  visuals.drawTranslucent(cmd);
  pfxGroup.drawTranslucent(cmd, fId);
  }

MeshObjects::Mesh WorldView::addView(std::string_view visual, int32_t headTex, int32_t teethTex, int32_t bodyColor) {
  if(auto mesh=Resources::loadMesh(visual))
    return MeshObjects::Mesh(objGroup,*mesh,headTex,teethTex,bodyColor,false);
  return MeshObjects::Mesh();
  }

MeshObjects::Mesh WorldView::addView(const ProtoMesh* mesh) {
  if(mesh!=nullptr) {
    bool staticDraw = false;
    return MeshObjects::Mesh(objGroup,*mesh,0,0,0,staticDraw);
    }
  return MeshObjects::Mesh();
  }

MeshObjects::Mesh WorldView::addItmView(std::string_view visual, int32_t material) {
  if(auto mesh=Resources::loadMesh(visual))
    return MeshObjects::Mesh(objGroup,*mesh,material,0,0,true);
  return MeshObjects::Mesh();
  }

MeshObjects::Mesh WorldView::addAtachView(const ProtoMesh::Attach& visual, const int32_t version) {
  return MeshObjects::Mesh(objGroup,visual,version,false);
  }

MeshObjects::Mesh WorldView::addStaticView(const ProtoMesh* mesh, bool staticDraw) {
  if(mesh!=nullptr)
    return MeshObjects::Mesh(objGroup,*mesh,0,0,0,staticDraw);
  return MeshObjects::Mesh();
  }

MeshObjects::Mesh WorldView::addStaticView(std::string_view visual) {
  if(auto mesh=Resources::loadMesh(visual))
    return MeshObjects::Mesh(objGroup,*mesh,0,0,0,true);
  return MeshObjects::Mesh();
  }

MeshObjects::Mesh WorldView::addDecalView(const asset::VisualDecal& vob) {
  if(auto mesh=Resources::decalMesh(vob))
    return MeshObjects::Mesh(objGroup,*mesh,0,0,0,true);
  return MeshObjects::Mesh();
  }

LightGroup::Light WorldView::addLight(const asset::VLight& vob, const uint64_t timeOffset) {
  auto l = gLights.add(vob);
  l.setTimeOffset(timeOffset);
  return l;
  }

LightGroup::Light WorldView::addLight(std::string_view preset, const uint64_t timeOffset) {
  auto l = gLights.add(preset);
  l.setTimeOffset(timeOffset);
  return l;
  }

void WorldView::dbgClusters(Tempest::Painter& p, Vec2 wsz) {
  visuals.dbgClusters(p, wsz);
  }

const DrawClusters& WorldView::clusters() const {
  return visuals.clusters();
  }

const DrawCommands& WorldView::drawCommands() const {
  return visuals.drawCommands();
  }

const DrawBuckets& WorldView::drawBuckets() const {
  return visuals.drawBuckets();
  }

const Tempest::StorageBuffer& WorldView::instanceSsbo() const {
  return visuals.instanceSsbo();
  }

bool WorldView::updateLights(const gtime gameTime) {
  gSky.updateLight(gameTime);
  if(!gLights.updateLights())
    return false;
  sGlobal.lights = &gLights.lightsSsbo();
  return true;
  }

bool WorldView::updateRtScene() {
  if(!Shaders::options().doRtScene)
    return false;
  if(!visuals.updateRtScene(sGlobal.rtScene))
    return false;
  return true;
  }

void WorldView::postFrameupdate() {
  visuals.postFrameupdate();
  }

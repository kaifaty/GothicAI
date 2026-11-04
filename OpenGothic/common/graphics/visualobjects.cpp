#include "visualobjects.h"

#include <Tempest/Log>
#include <algorithm>
#include <cmath>
#include <cassert>
#include <atomic>

#include "graphics/mesh/submesh/animmesh.h"
#include "graphics/mesh/submesh/staticmesh.h"
#include "gothic.h"
#include "frameprofile.h"
#include "loadprobe.h"
#include "physics/worldwind.h"
#include "world/chunkresidency.h"

using namespace Tempest;

static RtScene::Category toRtCategory(DrawCommands::Type t) {
  switch (t) {
    case DrawCommands::Landscape: return RtScene::Landscape;
    case DrawCommands::Static:    return RtScene::Static;
    case DrawCommands::Movable:   return RtScene::Movable;
    case DrawCommands::Animated:  return RtScene::None;
    case DrawCommands::Pfx:       return RtScene::None;
    case DrawCommands::Morph:     return RtScene::None;
    }
  return RtScene::None;
  }

static uint64_t vsmHashBytes(uint64_t h, const void* data, size_t size) {
  auto* b = reinterpret_cast<const uint8_t*>(data);
  for(size_t i=0; i<size; ++i) {
    h ^= b[i];
    h *= 1099511628211ull;
    }
  return h;
  }

static std::atomic<uint64_t> vsmEpochs{0};


struct VisualObjects::InstanceDesc {
  void setPosition(const Tempest::Matrix4x4& m) {
    for(int i=0; i<4; ++i)
      for(int r=0; r<3; ++r)
        pos[i][r] = m.at(i,r);
    }
  float    pos[4][3] = {};
  float    fatness   = 0;
  uint32_t animPtr   = 0;
  uint32_t deformPtr = 0;
  uint32_t deformCount = 0;
  };

struct VisualObjects::MorphDesc final {
  uint32_t indexOffset = 0;
  uint32_t sample0     = 0;
  uint32_t sample1     = 0;
  uint16_t alpha       = 0;
  uint16_t intensity   = 0;
  };

struct VisualObjects::MorphData {
  MorphDesc morph[Resources::MAX_MORPH_LAYERS];
  };


void VisualObjects::Item::setObjMatrix(const Tempest::Matrix4x4& pos) {
  setObjMatrix(pos,Vec3(pos[3][0],pos[3][1],pos[3][2]));
  }

void VisualObjects::Item::setObjMatrix(const Tempest::Matrix4x4& pos, const Vec3& texturePosition) {
  if(owner!=nullptr) {
    auto& obj = owner->objects[id];
    if(obj.pos!=pos)
      owner->updateRtAs(id);
    obj.pos = pos;
    if(obj.textureCategory==Resources::TextureCategory::Static)
      setTexturePosition(texturePosition);
    owner->updateInstance(id);
    }
  }

void VisualObjects::Item::setTexturePosition(const Vec3& position) {
  if(owner==nullptr)
    return;
  auto& obj=owner->objects[id];
  if(obj.textureCategory!=Resources::TextureCategory::Static)
    return;
  const auto chunk=ChunkId::fromPosition(position.x,position.z);
  if(obj.textureChunkRegistered && obj.textureChunk==std::pair{chunk.x,chunk.z})
    return;
  FrameProfile::Scope timing(FrameProfile::Zone::TextureDemand);
  std::lock_guard<std::recursive_mutex> guard(owner->textureSync);
  obj.texturePosition=position;
  owner->updateTextureDemand(id);
  }

void VisualObjects::Item::setTextureOwner(const Vec3& position, bool spatial) {
  if(owner==nullptr)
    return;
  auto& obj=owner->objects[id];
  const auto chunk=ChunkId::fromPosition(position.x,position.z);
  if(obj.textureCategory==Resources::TextureCategory::Dynamic && obj.textureWorldOwner==spatial &&
     (!spatial || (obj.textureChunkRegistered && obj.textureChunk==std::pair{chunk.x,chunk.z})))
    return;
  FrameProfile::Scope timing(FrameProfile::Zone::TextureDemand);
  std::lock_guard<std::recursive_mutex> guard(owner->textureSync);
  owner->setTextureDemand(obj,false);
  if(obj.textureChunkRegistered) {
    if(auto found=owner->textureChunks.find(obj.textureChunk);found!=owner->textureChunks.end()) {
      found->second.erase(id);
      if(found->second.empty())
        owner->textureChunks.erase(found);
      }
    obj.textureChunkRegistered=false;
    }
  obj.textureCategory=Resources::TextureCategory::Dynamic;
  obj.textureChunkedLandscape=false;
  obj.textureWorldOwner=spatial;
  obj.texturePosition=position;
  owner->updateTextureDemand(id);
  }

void VisualObjects::Item::setTextureCategory(Resources::TextureCategory category) {
  if(owner==nullptr)
    return;
  auto& obj=owner->objects[id];
  if(obj.textureCategory==category && !obj.textureWorldOwner)
    return;
  FrameProfile::Scope timing(FrameProfile::Zone::TextureDemand);
  std::lock_guard<std::recursive_mutex> guard(owner->textureSync);
  owner->setTextureDemand(obj,false);
  if(obj.textureChunkRegistered) {
    if(auto found=owner->textureChunks.find(obj.textureChunk);found!=owner->textureChunks.end()) {
      found->second.erase(id);
      if(found->second.empty())
        owner->textureChunks.erase(found);
      }
    obj.textureChunkRegistered=false;
    }
  obj.textureCategory=category;
  obj.textureWorldOwner=false;
  if(category!=Resources::TextureCategory::Landscape)
    obj.textureChunkedLandscape=false;
  owner->updateTextureDemand(id);
  }

void VisualObjects::Item::setAsGhost(bool g) {
  if(owner!=nullptr) {
    owner->setAsGhost(id, g);
    }
  }

void VisualObjects::Item::setFatness(float f) {
  if(owner!=nullptr) {
    if(owner->objects[id].fatness == f)
      return;
    owner->objects[id].fatness = f;
    owner->updateInstance(id);
    }
  }

void VisualObjects::Item::setWind(asset::AnimationType m, float intensity) {
  if(owner==nullptr)
    return;

  if(intensity!=0 && m==asset::AnimationType::NONE)
    m = asset::AnimationType::WIND_ALT;

  if(intensity==0)
    m = asset::AnimationType::NONE;

  auto& obj = owner->objects[id];

  const auto prev = obj.wind;
  obj.wind          = m;
  obj.windIntensity = intensity;

  if(prev==m)
    return;

  if(prev!=asset::AnimationType::NONE)
    owner->objectsWind.erase(id);
  if(m!=asset::AnimationType::NONE)
    owner->objectsWind.insert(id);
  owner->setVsmMovable(id, m!=asset::AnimationType::NONE || obj.deformCount>0);
  }

void VisualObjects::Item::setVisible(bool visible) {
  if(owner==nullptr)
    return;
  auto& obj = owner->objects[id];
  if(obj.visible==visible || obj.isEmpty())
    return;
  obj.visible = visible;
  const auto meshlets = obj.iboLen/PackedMesh::MaxInd;
  const auto count = obj.type==DrawCommands::Landscape ? meshlets : 1u;
  owner->vsmChangedClusters(obj);
  for(uint32_t i=0;i<count;++i)
    owner->clustersMem[obj.clusterId+i].meshletCount = visible ? (obj.type==DrawCommands::Landscape ? 1u : meshlets) : 0u;
  owner->clustersMem.markClusters(obj.clusterId,count);
  owner->updateRtAs(id);
  }

void VisualObjects::Item::startMMAnim(std::string_view anim, float intensity, uint64_t timeUntil, bool freeOnly) {
  if(owner!=nullptr)
    owner->startMMAnim(id, anim, intensity, timeUntil, freeOnly);
  }

void VisualObjects::Item::setDeformation(const std::vector<Tempest::Vec4>& vertices) {
  if(owner==nullptr)
    return;
  auto& obj = owner->objects[id];
  const auto count = uint32_t(vertices.size()/2);
  if(count==0) {
    if(obj.deformCount==0)
      return;
    obj.objDeform = InstanceStorage::Id();
    obj.deformCount = 0;
    owner->setVsmMovable(id, obj.wind!=asset::AnimationType::NONE);
    if(auto mesh=obj.bucketId->staticMesh) {
      owner->clustersMem[obj.clusterId].r = mesh->bbox.rConservative+obj.bucketId->mat.waveMaxAmplitude;
      owner->clustersMem.markClusters(obj.clusterId);
      }
    owner->updateInstance(id);
    return;
    }
  {
    // a cloth's shape changes every frame: the VSM lamp cache draws the faces round it again (isVsmDynamic)
    std::lock_guard<std::mutex> guard(owner->vsmSync);
    owner->objectsDynamic.insert(id);
  }
  const auto size = vertices.size()*sizeof(Tempest::Vec4);
  const bool changed = owner->instanceMem.realloc(obj.objDeform,size);
  obj.objDeform.set(vertices.data(),0,size);
  // The authored bounds are shared by every instance. Grow this instance's
  // sphere to retain cloth that folds beyond the original flat mesh bounds.
  float radius = 0;
  for(size_t i=0;i<vertices.size();i+=2) {
    const auto& p = vertices[i];
    Vec3 point(p.x,p.y,p.z);
    float zero = 0;
    obj.pos.project(point.x,point.y,point.z,zero);
    radius = std::max(radius,point.length());
    }
  if(owner->clustersMem[obj.clusterId].r<radius) {
    owner->clustersMem[obj.clusterId].r = radius;
    owner->clustersMem.markClusters(obj.clusterId);
    }
  if(changed || obj.deformCount!=count) {
    obj.deformCount = count;
    owner->setVsmMovable(id, true);
    owner->updateInstance(id);
    }
  }

void VisualObjects::Item::setMMShape(std::string_view anim, uint32_t frame0, uint32_t frame1, float alpha, float intensity) {
  if(owner!=nullptr)
    owner->setMMShape(id, anim, frame0, frame1, alpha, intensity);
  }

float VisualObjects::Item::morphWeight(std::string_view anim) {
  return owner!=nullptr ? owner->morphWeight(id, anim) : 0.f;
  }

VisualObjects::MorphSample VisualObjects::Item::morphSample(std::string_view name, uint64_t tick) const {
  if(owner==nullptr)
    return {};
  auto& obj = owner->objects[id];
  const auto* mesh = obj.bucketId->staticMesh;
  if(mesh==nullptr || mesh->morph.anim==nullptr)
    return {};
  for(const auto& ani:obj.morphAnim) {
    if(ani.id>=mesh->morph.anim->size() || ani.timeUntil<tick)
      continue;
    const auto& anim = (*mesh->morph.anim)[ani.id];
    if(anim.name!=name || anim.numFrames==0 || ani.intensity<=0.f)
      continue;
    const auto time = tick-ani.timeStart;
    float weight = 1.f;
    if(!ani.manual) {
      if(anim.blendIn>0.f)
        weight = std::min(weight,float(time)*anim.blendIn);
      if(anim.blendOut>0.f && ani.timeUntil!=uint64_t(-1))
        weight = std::min(weight,float(ani.timeUntil-tick)*anim.blendOut);
      }
    const auto quantize = [](float x) { return float(uint16_t(std::clamp(x,0.f,1.f)*uint16_t(-1)))/float(uint16_t(-1)); };
    MorphSample sample;
    sample.frame0 = ani.manual ? ani.frame0 : uint32_t((time/anim.tickPerFrame+0)%anim.numFrames);
    sample.frame1 = ani.manual ? ani.frame1 : uint32_t((time/anim.tickPerFrame+1)%anim.numFrames);
    sample.alpha = quantize(ani.manual ? ani.alpha : float(time%anim.tickPerFrame)/float(anim.tickPerFrame));
    sample.intensity = quantize(ani.intensity*std::clamp(weight,0.f,1.f));
    return sample;
    }
  return {};
  }

const Material& VisualObjects::Item::material() const {
  if(owner==nullptr) {
    static Material m;
    return m;
    }
  auto& bx = *owner->objects[id].bucketId;
  return bx.mat;
  }

const Bounds& VisualObjects::Item::bounds() const {
  static Bounds dummy;
  if(owner==nullptr)
    return dummy;
  auto& bx = *owner->objects[id].bucketId;
  if(bx.staticMesh!=nullptr)
    return bx.staticMesh->bbox;
  if(bx.animMesh!=nullptr)
    return bx.animMesh->bbox;
  return dummy;
  }

Matrix4x4 VisualObjects::Item::position() const {
  if(owner!=nullptr)
    return owner->objects[id].pos;
  return Matrix4x4();
  }

const StaticMesh* VisualObjects::Item::mesh() const {
  if(owner==nullptr)
    return nullptr;
  auto& bx = *owner->objects[id].bucketId;
  return bx.staticMesh;
  }

std::pair<uint32_t, uint32_t> VisualObjects::Item::meshSlice() const {
  if(owner==nullptr)
    return std::pair<uint32_t, uint32_t>(0,0);
  auto& obj = owner->objects[id];
  return std::make_pair(obj.iboOff, obj.iboLen);
  }


VisualObjects::VisualObjects(const SceneGlobals& scene, const std::pair<Vec3, Vec3>& bbox)
    : scene(scene), drawCmd(*this, bucketsMem, clustersMem, scene) {
  objectsMorph.reserve(1024);
  }

VisualObjects::~VisualObjects() {
  std::lock_guard<std::recursive_mutex> guard(textureSync);
  if(textureResidency) {
    textureResidency->unsubscribe(textureSubscription);
    textureResidency->unsubscribe(textureFocusSubscription);
    }
  for(auto& object:objects)
    if(!object.isEmpty())
      setTextureDemand(object,false);
  }

void VisualObjects::setTextureResidency(ChunkResidency* residency) {
  FrameProfile::Scope timing(FrameProfile::Zone::TextureDemand);
  std::lock_guard<std::recursive_mutex> guard(textureSync);
  if(textureResidency) {
    textureResidency->unsubscribe(textureSubscription);
    textureResidency->unsubscribe(textureFocusSubscription);
    }
  textureResidency=residency;
  if(textureResidency) {
    textureSubscription=textureResidency->subscribe([this](ChunkId chunk, ChunkResidency::State, ChunkResidency::State) {
      FrameProfile::Scope timing(FrameProfile::Zone::TextureDemand);
      std::lock_guard<std::recursive_mutex> guard(textureSync);
      const auto found=textureChunks.find({chunk.x,chunk.z});
      if(found==textureChunks.end())
        return;
      for(auto id:found->second)
        updateTextureDemand(id);
      });
    textureFocusSubscription=textureResidency->subscribeFocus([this] { updateTextureFocus(); });
    }
  for(size_t id=0;id<objects.size();++id)
    if(!objects[id].isEmpty())
      updateTextureDemand(id);
  }

bool VisualObjects::textureChunkActive(const Vec3& position) const {
  std::lock_guard<std::recursive_mutex> guard(textureSync);
  if(!textureResidency)
    return true;
  const auto chunk=ChunkId::fromPosition(position.x,position.z);
  const auto index=textureResidency->indexOf(chunk);
  if(index!=size_t(-1) && textureResidency->table().entries[index].triangleCount!=0)
    return textureResidency->desired(index)!=ChunkResidency::State::Outside;
  // Some visuals occupy cells without landscape triangles.
  return true;
  }

ChunkResidency* VisualObjects::textureResidencyManager() const {
  std::lock_guard<std::recursive_mutex> guard(textureSync);
  return textureResidency;
  }

void VisualObjects::setTextureDemand(Object& object, bool active) {
  std::lock_guard<std::recursive_mutex> guard(textureSync);
  if(object.textureDemand==active)
    return;
  Resources::textureDemand(object.bucketId->mat,object.textureCategory,active,object.textureDistance);
  object.textureDemand=active;
  }

void VisualObjects::updateTextureDemand(size_t id) {
  std::lock_guard<std::recursive_mutex> guard(textureSync);
  auto& object=objects[id];
  if(object.isEmpty())
    return;
  if(object.textureCategory!=Resources::TextureCategory::Static && !object.textureChunkedLandscape && !object.textureWorldOwner) {
    object.textureDistance=Resources::TextureAlwaysFull;
    setTextureDemand(object,true);
    return;
    }
  const auto chunk=ChunkId::fromPosition(object.texturePosition.x,object.texturePosition.z);
  const std::pair key{chunk.x,chunk.z};
  if(!object.textureChunkRegistered || key!=object.textureChunk) {
    if(object.textureChunkRegistered)
      if(auto found=textureChunks.find(object.textureChunk);found!=textureChunks.end()) {
        found->second.erase(id);
        if(found->second.empty())
          textureChunks.erase(found);
        }
    object.textureChunk=key;
    textureChunks[key].insert(id);
    object.textureChunkRegistered=true;
    }
  const auto distance=textureResidency ? std::min(4u,textureResidency->textureDistance(chunk)) : 0;
  if(object.textureDistance!=distance) {
    setTextureDemand(object,false);
    object.textureDistance=distance;
    }
  // A landscape owner exists only while its package is published.
  setTextureDemand(object,object.textureWorldOwner || object.textureChunkedLandscape || textureChunkActive(object.texturePosition));
  }

void VisualObjects::updateTextureFocus() {
  FrameProfile::Scope timing(FrameProfile::Zone::TextureDemand);
  std::lock_guard<std::recursive_mutex> guard(textureSync);
  for(const auto& entry:textureChunks) {
    const ChunkId chunk{entry.first.first,entry.first.second};
    const auto distance=std::min(4u,textureResidency->textureDistance(chunk));
    if(entry.second.empty() || objects[*entry.second.begin()].textureDistance==distance)
      continue;
    for(auto id:entry.second)
      updateTextureDemand(id);
    }
  }

void VisualObjects::invalidate(const StaticMesh& mesh) {
  assert(std::none_of(objects.begin(),objects.end(),[&](const auto& object) {
    return !object.isEmpty() && object.bucketId->staticMesh==&mesh;
    }));
  bucketsMem.invalidate(mesh);
  }

void VisualObjects::updateInstance(size_t id, Matrix4x4* pos) {
  auto& obj = objects[id];
  if(obj.type==DrawCommands::Landscape)
    return;

  InstanceDesc d;
  d.setPosition(pos==nullptr ? obj.pos : *pos);
  d.animPtr = obj.animPtr;
  d.fatness = obj.fatness*0.5f;
  d.deformPtr = obj.objDeform.offsetId<uint32_t>();
  d.deformCount = obj.deformCount;
  obj.objInstance.set(&d, 0, sizeof(d));

  auto cId  = obj.clusterId;
  auto npos = Vec3(obj.pos[3][0], obj.pos[3][1], obj.pos[3][2]);
  if(pos==nullptr && !isVsmDynamic(obj) && scene.vsmEnabled) {
    // a caster moved, turned or changed its shape: the lamp faces that saw it before and those that see it now.
    // A drift below VsmMoveTolerance from where the faces last saw it (a physics body at rest jitters by
    // hundredths of a millimetre) is less than a texel of a face: it is gathered until it is more
    const float r      = clustersMem[cId].r;
    const Vec4  sphere = vsmSphere(obj, obj.pos);
    uint64_t h = 14695981039346656037ull;
    h = vsmHashBytes(h, &d.fatness, sizeof(d)-sizeof(d.pos));
    h = vsmHashBytes(h, &r, sizeof(r));
    if(!obj.vsmPlaced || obj.vsmHash!=h || vsmDisplacement(obj.vsmPos, obj.pos, std::max(r, sphere.w))>VsmMoveTolerance) {
      if(obj.vsmPlaced)
        vsmChanged(obj, obj.vsmSphere);
      obj.vsmSphere = sphere;
      obj.vsmPos    = obj.pos;
      obj.vsmHash   = h;
      obj.vsmPlaced = true;
      vsmChanged(obj, obj.vsmSphere);
      }
    }
  if(clustersMem[cId].pos != npos) {
    clustersMem[cId].pos = npos;
    clustersMem.markClusters(cId);
    }
  }

void VisualObjects::updateRtAs(size_t id) {
  auto& obj = objects[id];
  auto& mat = obj.bucketId->mat;

  bool useBlas = toRtCategory(obj.type)!=RtScene::None;
  if(mat.alpha==Material::Ghost)
    useBlas = false;
  if(mat.alpha!=Material::Solid && mat.alpha!=Material::AlphaTest && mat.alpha!=Material::Transparent)
    useBlas = false;

  if(!useBlas)
    return;

  auto& mesh = *obj.bucketId->staticMesh;
  if(auto b = mesh.blas(obj.iboOff, obj.iboLen)) {
    (void)b;
    notifyTlas(mat, toRtCategory(obj.type));
    }
  }

VisualObjects::Item VisualObjects::get(const StaticMesh& mesh, const Material& mat,
                                       size_t iboOff, size_t iboLen,
                                       bool staticDraw) {
  LoadProbe::Vobs::Scope probe("draw_instance");
  std::lock_guard<std::recursive_mutex> guard(textureSync);
  // return Item();
  // 64x64 meshlets
  assert(iboOff%PackedMesh::MaxInd==0);
  assert(iboLen%PackedMesh::MaxInd==0);

  if(mat.tex==nullptr) {
    Log::e("no texture?!");
    return VisualObjects::Item();
    }
  auto         type = (staticDraw ? DrawCommands::Static : DrawCommands::Movable);
  const size_t id     = implAlloc();

  Object& obj = objects[id];

  if(mesh.morph.anim!=nullptr) {
    type = DrawCommands::Morph;
    }

  obj.type      = type;
  obj.iboOff    = uint32_t(iboOff);
  obj.iboLen    = uint32_t(iboLen);
  obj.bucketId  = bucketsMem.alloc(mat, mesh);
  obj.vsmMovable = DrawCommands::isVsmMovable(obj.type);
  obj.cmdId     = drawCmd.commandId(mat, obj.type, obj.bucketId.toInt(), obj.vsmMovable);
  obj.clusterId = clusterId(*obj.bucketId, iboOff/PackedMesh::MaxInd, iboLen/PackedMesh::MaxInd, obj.bucketId.toInt(), obj.cmdId);
  obj.alpha     = mat.alpha;
  obj.timeShift = -scene.tickCount;

  if(obj.isEmpty())
    return Item(); // null command

  obj.textureCategory=staticDraw ? Resources::TextureCategory::Static : Resources::TextureCategory::Dynamic;
  { FrameProfile::Scope timing(FrameProfile::Zone::TextureDemand); updateTextureDemand(id); }
  obj.objInstance = instanceMem.alloc(sizeof(InstanceDesc));
  clustersMem[obj.clusterId].instanceId = obj.objInstance.offsetId<InstanceDesc>();

  if(type==DrawCommands::Morph) {
    {
      std::lock_guard<std::mutex> guard(vsmSync);
      objectsDynamic.insert(id);
    }
    obj.objMorphAnim = instanceMem.alloc(sizeof(MorphDesc)*Resources::MAX_MORPH_LAYERS);
    obj.animPtr      = obj.objMorphAnim.offsetId<MorphDesc>();
    const MorphData d = {};
    obj.objMorphAnim.set(&d, 0, sizeof(d));
    }

  updateInstance(id);
  updateRtAs(id);
  return Item(*this, id);
  }

VisualObjects::Item VisualObjects::get(const AnimMesh& mesh, const Material& mat,
                                       size_t iboOff, size_t iboLen,
                                       const InstanceStorage::Id& anim) {
  LoadProbe::Vobs::Scope probe("draw_instance");
  std::lock_guard<std::recursive_mutex> guard(textureSync);
  // return Item();
  // 64x64 meshlets
  assert(iboOff%PackedMesh::MaxInd==0);
  assert(iboLen%PackedMesh::MaxInd==0);

  if(mat.tex==nullptr) {
    Log::e("no texture?!");
    return VisualObjects::Item();
    }

  const size_t id = implAlloc();

  Object& obj = objects[id];

  obj.type      = DrawCommands::Type::Animated;
  obj.iboOff    = uint32_t(iboOff);
  obj.iboLen    = uint32_t(iboLen);
  obj.bucketId  = bucketsMem.alloc(mat, mesh);
  obj.vsmMovable = DrawCommands::isVsmMovable(obj.type);
  obj.cmdId     = drawCmd.commandId(mat, obj.type, obj.bucketId.toInt(), obj.vsmMovable);
  obj.clusterId = clusterId(*obj.bucketId, iboOff/PackedMesh::MaxInd, iboLen/PackedMesh::MaxInd, obj.bucketId.toInt(), obj.cmdId);
  obj.alpha     = mat.alpha;
  obj.timeShift = -scene.tickCount;

  if(obj.isEmpty())
    return Item(); // null command
  { FrameProfile::Scope timing(FrameProfile::Zone::TextureDemand); updateTextureDemand(id); }
  obj.animPtr     = anim.offsetId<Matrix4x4>();
  obj.objInstance = instanceMem.alloc(sizeof(InstanceDesc));
  {
    std::lock_guard<std::mutex> guard(vsmSync);
    objectsDynamic.insert(id);
  }
  clustersMem[obj.clusterId].instanceId = obj.objInstance.offsetId<InstanceDesc>();

  updateInstance(id);
  updateRtAs(id);
  return Item(*this, id);
  }

VisualObjects::Item VisualObjects::get(const StaticMesh& mesh, const Material& mat,
                                       size_t iboOff, size_t iboLen, const PackedMesh::Cluster* cluster,
                                       DrawCommands::Type type, const Vec3* texturePosition) {
  LoadProbe::Vobs::Scope probe("draw_instance");
  std::lock_guard<std::recursive_mutex> guard(textureSync);
  // return Item();
  // 64x64 meshlets
  assert(iboOff%PackedMesh::MaxInd==0);
  assert(iboLen%PackedMesh::MaxInd==0);

  const size_t id = implAlloc();

  Object& obj = objects[id];

  obj.type      = DrawCommands::Type::Landscape;
  obj.iboOff    = uint32_t(iboOff);
  obj.iboLen    = uint32_t(iboLen);
  obj.bucketId  = bucketsMem.alloc(mat, mesh);
  obj.cmdId     = drawCmd.commandId(mat, type, obj.bucketId.toInt(), false);
  obj.clusterId = clusterId(cluster, mesh.firstMeshlet()+iboOff/PackedMesh::MaxInd, iboLen/PackedMesh::MaxInd, obj.bucketId.toInt(), obj.cmdId);
  obj.alpha     = mat.alpha;
  obj.timeShift = -scene.tickCount;

  if(obj.isEmpty())
    return Item(); // null command
  vsmChangedClusters(obj);
  obj.textureCategory=Resources::TextureCategory::Landscape;
  obj.textureChunkedLandscape=texturePosition!=nullptr;
  if(texturePosition)
    obj.texturePosition=*texturePosition;
  { FrameProfile::Scope timing(FrameProfile::Zone::TextureDemand); updateTextureDemand(id); }
  updateRtAs(id);
  return Item(*this, id);
  }

size_t VisualObjects::implAlloc() {
  if(!objectsFree.empty()) {
    auto id = *objectsFree.begin();
    objectsFree.erase(objectsFree.begin());
    return id;
    }

  objects.resize(objects.size()+1);
  return objects.size()-1;
  }

void VisualObjects::free(size_t id) {
  FrameProfile::Scope timing(FrameProfile::Zone::TextureDemand);
  std::lock_guard<std::recursive_mutex> guard(textureSync);
  Object& obj = objects[id];
  setTextureDemand(obj,false);
  if(obj.textureChunkRegistered) {
    if(auto found=textureChunks.find(obj.textureChunk);found!=textureChunks.end()) {
      found->second.erase(id);
      if(found->second.empty())
        textureChunks.erase(found);
      }
    }
  timing.finish();

  const uint32_t meshletCount = (obj.iboLen/PackedMesh::MaxInd);
  const uint32_t numCluster = (obj.type==DrawCommands::Landscape ? meshletCount : 1);

  if(obj.visible && !obj.isEmpty())
    vsmChangedClusters(obj);
  {
    std::lock_guard<std::mutex> guard(vsmSync);
    objectsDynamic.erase(id);
  }
  drawCmd.addClusters(obj.cmdId, -meshletCount);
  clustersMem.free(obj.clusterId, numCluster);
  updateRtAs(id);

  if(obj.wind!=asset::AnimationType::NONE)
    objectsWind.erase(id);
  if(obj.type==DrawCommands::Morph)
    objectsMorph.erase(id);

  obj = Object();
  while(objects.size()>0) {
    if(!objects.back().isEmpty())
      break;
    objects.pop_back();
    objectsFree.erase(objects.size());
    }
  if(id<objects.size())
    objectsFree.insert(id);
  }

InstanceStorage::Id VisualObjects::alloc(size_t size) {
  return instanceMem.alloc(size);
  }

bool VisualObjects::realloc(InstanceStorage::Id& id, size_t size) {
  return instanceMem.realloc(id, size);
  }

const Tempest::StorageBuffer& VisualObjects::instanceSsbo() const {
  return instanceMem.ssbo();
  }

uint32_t VisualObjects::clusterId(const PackedMesh::Cluster* cx, size_t firstMeshlet, size_t meshletCount, uint16_t bucketId, uint16_t commandId) {
  if(commandId==uint16_t(-1))
    return uint32_t(-1);

  const auto ret = clustersMem.alloc(cx, firstMeshlet, meshletCount, bucketId, commandId);
  drawCmd.addClusters(commandId, uint32_t(meshletCount));
  return uint32_t(ret);
  }

uint32_t VisualObjects::clusterId(const DrawBuckets::Bucket& bucket, size_t firstMeshlet, size_t meshletCount, uint16_t bucketId, uint16_t commandId) {
  if(commandId==uint16_t(-1))
    return uint32_t(-1);

  const auto ret = clustersMem.alloc(bucket, firstMeshlet, meshletCount, bucketId, commandId);
  drawCmd.addClusters(commandId, uint32_t(meshletCount));
  return uint32_t(ret);
  }

void VisualObjects::startMMAnim(size_t i, std::string_view animName, float intensity, uint64_t timeUntil, bool freeOnly) {
  auto& obj        = objects[i];
  auto  staticMesh = obj.bucketId->staticMesh;
  if(staticMesh==nullptr || staticMesh->morph.anim==nullptr)
    return;

  auto&  anim = *staticMesh->morph.anim;
  size_t id   = size_t(-1);
  for(size_t i=0; i<anim.size(); ++i)
    if(anim[i].name==animName) {
      id = i;
      break;
      }

  if(id==size_t(-1))
    return;

  objectsMorph.insert(i);

  const uint64_t now  = scene.tickCount;
  auto&          m    = anim[id];
  const bool     stop = timeUntil<now;  // the anim is stopped (a time of 0)
  if(timeUntil==uint64_t(-1) && m.duration>0)
    timeUntil = now + m.duration;
  // held for its time, then fades out
  if(!stop && timeUntil!=uint64_t(-1) && m.blendOut>0)
    timeUntil += uint64_t(1.f/m.blendOut);

  // fades out from where it is now
  auto fadeOut = [&](MorphAnim& a) {
    const auto& am = anim[a.id];
    if(a.manual || am.blendOut<=0) {
      a.timeUntil = 0;
      return;
      }
    a.timeUntil = std::min(a.timeUntil, now + uint64_t(morphWeight(obj,a)/am.blendOut));
    };

  // another anim of the layer gives way: S_ANGRY replaces S_FRIENDLY
  if(!stop) {
    for(auto& a:obj.morphAnim) {
      if(a.id==id || a.timeUntil<now || a.manual || anim[a.id].layer!=m.layer)
        continue;
      fadeOut(a);
      }
    }

  // the anim is on already (or still fading out): it goes on from its weight
  for(auto& a:obj.morphAnim) {
    if(a.id!=id || a.timeUntil<now)
      continue;
    if(stop) {
      fadeOut(a);
      return;
      }
    if(m.blendIn>0) {
      const float w = a.manual ? 1.f : morphWeight(obj,a);
      a.timeStart = now - std::min(now, uint64_t(std::ceil(w/m.blendIn)));
      }
    a.timeUntil = timeUntil;
    a.intensity = intensity;
    a.manual    = false;
    return;
    }
  if(stop)
    return;
  if(freeOnly && std::none_of(std::begin(obj.morphAnim),std::end(obj.morphAnim),[&](const MorphAnim& a){ return a.timeUntil<now; }))
    return;

  auto& ani = morphSlot(obj);
  ani.id        = id;
  ani.timeStart = now;
  ani.timeUntil = timeUntil;
  ani.intensity = intensity;
  ani.manual    = false;
  }

float VisualObjects::morphWeight(Object& obj, const MorphAnim& a) {
  const uint64_t now = scene.tickCount;
  if(a.timeUntil<now)
    return 0;
  if(a.manual)
    return 1;
  const auto& m = (*obj.bucketId->staticMesh->morph.anim)[a.id];
  float w = 1;
  if(m.blendIn>0)
    w = std::min(w, float(now-a.timeStart)*m.blendIn);
  if(m.blendOut>0 && a.timeUntil!=uint64_t(-1))
    w = std::min(w, float(a.timeUntil-now)*m.blendOut);
  return std::clamp(w, 0.f, 1.f);
  }

float VisualObjects::morphWeight(size_t i, std::string_view animName) {
  auto& obj = objects[i];
  if(obj.isEmpty() || obj.bucketId->staticMesh==nullptr || obj.bucketId->staticMesh->morph.anim==nullptr)
    return 0;
  auto& anim = *obj.bucketId->staticMesh->morph.anim;
  float w    = 0;
  for(auto& a:obj.morphAnim)
    if(a.id<anim.size() && anim[a.id].name==animName)
      w = std::max(w, a.intensity*morphWeight(obj,a));
  return w;
  }

auto VisualObjects::morphSlot(Object& obj) -> MorphAnim& {
  // a free slot, else the faintest anim that is not set by its owner
  MorphAnim* slot = nullptr;
  float      best = 2;
  for(auto& a:obj.morphAnim) {
    if(a.timeUntil<scene.tickCount)
      return a;
    if(a.manual)
      continue;
    const float w = morphWeight(obj,a)*a.intensity;
    if(w<best) {
      best = w;
      slot = &a;
      }
    }
  return slot!=nullptr ? *slot : obj.morphAnim[0];
  }

void VisualObjects::setMMShape(size_t i, std::string_view animName, uint32_t frame0, uint32_t frame1, float alpha, float intensity) {
  auto& obj        = objects[i];
  auto  staticMesh = obj.bucketId->staticMesh;
  if(staticMesh==nullptr || staticMesh->morph.anim==nullptr)
    return;

  auto&  anim = *staticMesh->morph.anim;
  size_t id   = size_t(-1);
  for(size_t r=0; r<anim.size(); ++r)
    if(anim[r].name==animName) {
      id = r;
      break;
      }
  if(id==size_t(-1) || anim[id].numFrames==0)
    return;

  // the layer of the anim, or a free slot, or the faintest one
  auto&      m    = anim[id];
  MorphAnim* slot = nullptr;
  for(auto& a:obj.morphAnim)
    if(a.timeUntil>=scene.tickCount && anim[a.id].layer==m.layer) {
      slot = &a;
      break;
      }
  if(slot==nullptr)
    slot = &morphSlot(obj);

  const uint32_t last = uint32_t(m.numFrames-1);
  if(slot->id!=id || slot->timeUntil<scene.tickCount)
    slot->timeStart = scene.tickCount;
  slot->id        = id;
  slot->timeUntil = scene.tickCount + 250;  // the owner sets the shape every frame; if it stops, the shape goes
  slot->intensity = std::clamp(intensity, 0.f, 1.f);
  slot->manual    = true;
  slot->frame0    = std::min(frame0, last);
  slot->frame1    = std::min(frame1, last);
  slot->alpha     = std::clamp(alpha, 0.f, 1.f);
  objectsMorph.insert(i);
  }

void VisualObjects::setVsmMovable(size_t id, bool movable) {
  // a wind-blown or deformed object changes every frame: in the VSM lamp-face cache it is drawn over a face's
  // stored static part, as a movable one, rather than drawing the whole face again; it gets a command of its class
  auto& obj = objects[id];
  movable = movable || DrawCommands::isVsmMovable(obj.type);
  if(obj.isEmpty() || obj.vsmMovable==movable || obj.cmdId==uint16_t(-1))
    return;
  auto& cx  = drawCmd[obj.cmdId];
  auto  mat = obj.bucketId->mat;
  const auto type = cx.type;

  vsmChangedClusters(obj, true); // leaves its class
  const uint32_t meshletCount = (obj.iboLen/PackedMesh::MaxInd);
  drawCmd.addClusters(obj.cmdId, -meshletCount);
  obj.vsmMovable = movable;
  obj.cmdId      = drawCmd.commandId(mat, type, obj.bucketId.toInt(), obj.vsmMovable);
  drawCmd.addClusters(obj.cmdId, +meshletCount);
  vsmChangedClusters(obj, true); // enters the other

  const uint32_t numCluster = (obj.type==DrawCommands::Landscape ? meshletCount : 1);
  for(size_t i=0; i<numCluster; ++i) {
    clustersMem[obj.clusterId + i].commandId = obj.cmdId;
    clustersMem.markClusters(obj.clusterId + i);
    }
  }

void VisualObjects::setAsGhost(size_t id, bool g) {
  auto& obj= objects[id];
  if(obj.isGhost==g)
    return;

  auto& bx  = *obj.bucketId;
  auto& cx  = drawCmd[obj.cmdId];
  auto  mat = bx.mat;

  const uint32_t meshletCount = (obj.iboLen/PackedMesh::MaxInd);
  drawCmd.addClusters(obj.cmdId, -meshletCount);

  mat.alpha   = g ? Material::Ghost : obj.alpha;
  obj.isGhost = g;
  vsmChangedClusters(obj, true); // it casts before or after

  if(bx.staticMesh!=nullptr)
    obj.bucketId = bucketsMem.alloc(mat, *bx.staticMesh); else
    obj.bucketId = bucketsMem.alloc(mat, *bx.animMesh);

  obj.cmdId = drawCmd.commandId(mat, cx.type, obj.bucketId.toInt(), obj.vsmMovable);
  drawCmd.addClusters(obj.cmdId, +meshletCount);

  const uint32_t numCluster = (obj.type==DrawCommands::Landscape ? meshletCount : 1);
  for(size_t i=0; i<numCluster; ++i) {
    clustersMem[obj.clusterId + i].commandId = obj.cmdId;
    clustersMem[obj.clusterId + i].bucketId  = obj.bucketId.toInt();
    clustersMem.markClusters(obj.clusterId + i);
    }
  }

void VisualObjects::prepareGlobals(Encoder<CommandBuffer>& enc, uint8_t fId) {
  instanceMem.commit(enc, fId);
  clustersMem.commit(enc, fId);
  drawCmd.commit(enc);
  bucketsMem.commit(enc, fId);
  commitVsmChanges(fId);
  }

float VisualObjects::vsmDisplacement(const Matrix4x4& a, const Matrix4x4& b, float r) {
  // how far a point within r of the origin may have gone: the translation, plus the change of the rest of the
  // matrix (its Frobenius norm bounds the change of any vector) times r
  float rot = 0;
  for(int c=0; c<3; ++c)
    for(int i=0; i<3; ++i) {
      const float v = a.at(c,i)-b.at(c,i);
      rot += v*v;
      }
  const Vec3 t = Vec3(a.at(3,0)-b.at(3,0), a.at(3,1)-b.at(3,1), a.at(3,2)-b.at(3,2));
  return t.length() + std::sqrt(rot)*std::max(r, 0.f);
  }

bool VisualObjects::isVsmDynamic(const Object& obj) const {
  return obj.type==DrawCommands::Animated || obj.type==DrawCommands::Morph ||
         obj.wind!=asset::AnimationType::NONE || obj.deformCount>0;
  }

Vec4 VisualObjects::vsmSphere(const Object& obj, const Matrix4x4& m) const {
  // a static mesh: round its box (a tree's origin is at its foot: the cluster's sphere round the origin is half as
  // wide again); an animated one: the cluster's sphere
  auto& c  = const_cast<DrawClusters&>(clustersMem)[obj.clusterId];
  auto& bk = const_cast<Object&>(obj).bucketId;
  if(bk->staticMesh==nullptr)
    return Vec4(c.pos.x, c.pos.y, c.pos.z, c.r);
  auto& bb  = bk->staticMesh->bbox.bbox;
  Vec3  cen = (bb[0]+bb[1])*0.5f;
  float w   = 1;
  m.project(cen.x, cen.y, cen.z, w);
  float scale = 0;
  for(int i=0; i<3; ++i)
    scale = std::max(scale, Vec3(m.at(i,0), m.at(i,1), m.at(i,2)).length());
  const float r = (bb[1]-bb[0]).length()*0.5f*scale + bk->mat.waveMaxAmplitude;
  return Vec4(cen.x, cen.y, cen.z, r);
  }

bool VisualObjects::castsVsm(const Object& obj) const {
  // as DrawCommands::drawVsm draws: solid and alpha-tested materials (a ghost, water, glass cast nothing)
  if(obj.isEmpty())
    return false;
  const auto alpha = const_cast<Object&>(obj).bucketId->mat.alpha;
  return alpha==Material::Solid || alpha==Material::AlphaTest;
  }

void VisualObjects::vsmChanged(const Object& obj, const Vec4& sphere, bool force) {
  if(!scene.vsmEnabled || sphere.w<0 || !(force || castsVsm(obj)))
    return;
  auto& dst = vsmMoved[obj.vsmMovable ? 1 : 0];
  std::lock_guard<std::mutex> guard(vsmSync);
  if(dst.size()<=VsmMaxChanges)
    dst.push_back(sphere);
  }

void VisualObjects::vsmChangedClusters(const Object& obj, bool force) {
  if(!scene.vsmEnabled || obj.isEmpty() || obj.clusterId==uint32_t(-1))
    return;
  const uint32_t first = obj.clusterId;
  const uint32_t count = obj.type==DrawCommands::Landscape ? obj.iboLen/PackedMesh::MaxInd : 1u;
  if(count==0)
    return;
  if(count==1) {
    vsmChanged(obj, vsmSphere(obj, obj.pos), force);
    return;
    }
  // a landscape piece: one sphere round its meshlets
  Vec3 lo = clustersMem[first].pos, hi = lo;
  for(uint32_t i=0; i<count; ++i) {
    auto& c = clustersMem[first+i];
    lo = Vec3(std::min(lo.x, c.pos.x-c.r), std::min(lo.y, c.pos.y-c.r), std::min(lo.z, c.pos.z-c.r));
    hi = Vec3(std::max(hi.x, c.pos.x+c.r), std::max(hi.y, c.pos.y+c.r), std::max(hi.z, c.pos.z+c.r));
    }
  const Vec3 cen = (lo+hi)*0.5f;
  vsmChanged(obj, Vec4(cen.x, cen.y, cen.z, (hi-lo).length()*0.5f), force);
  }

void VisualObjects::commitVsmChanges(uint8_t fId) {
  vsmLast.serial++;
  if(vsmLast.epoch==0)
    vsmLast.epoch = ++vsmEpochs;
  vsmLast.count       = 0;
  vsmLast.staticCount = 0;
  vsmLast.spheres     = nullptr;
  vsmLast.overflow    = false;

  std::vector<Vec4> moved[2];
  {
    std::lock_guard<std::mutex> guard(vsmSync);
    std::swap(moved[0], vsmMoved[0]);
    std::swap(moved[1], vsmMoved[1]);
    vsmLast.moved = uint32_t(moved[0].size() + moved[1].size());
    if(!scene.vsmEnabled) {
      vsmDynamicPrev[0].clear();
      vsmDynamicPrev[1].clear();
      return;
      }

    // the casters that change every frame where they were last frame (whatever they drew there) and where they
    // are now: the animated and morphed ones are movable; the wind-blown and deformed ones by their draw
    std::vector<Vec4> now[2];
    auto add = [&](size_t id) {
      auto& obj = objects[id];
      if(!obj.visible || !castsVsm(obj) || obj.clusterId==uint32_t(-1))
        return;
      if(clustersMem[obj.clusterId].r<0)
        return;
      // a wind-blown tree sways by a part of its size round where it stands
      Vec4 sphere = vsmSphere(obj, obj.pos);
      if(obj.wind!=asset::AnimationType::NONE)
        sphere.w *= 1.1f;
      now[obj.vsmMovable ? 1 : 0].push_back(sphere);
      };
    for(auto id:objectsDynamic)
      add(id);
    for(auto id:objectsWind)
      add(id);

    vsmUpload.clear();
    for(int k=0; k<2; ++k) {
      vsmUpload.insert(vsmUpload.end(), moved[k].begin(), moved[k].end());
      vsmUpload.insert(vsmUpload.end(), vsmDynamicPrev[k].begin(), vsmDynamicPrev[k].end());
      vsmUpload.insert(vsmUpload.end(), now[k].begin(), now[k].end());
      if(k==0)
        vsmLast.staticCount = uint32_t(vsmUpload.size());
      vsmDynamicPrev[k] = std::move(now[k]);
      }
  }

  if(vsmUpload.size()>VsmMaxChanges) {
    vsmLast.overflow = true;
    return;
    }
  if(vsmUpload.empty())
    return;

  auto& device = Resources::device();
  auto& buf    = vsmSpheres[fId];
  const size_t size = vsmUpload.size()*sizeof(Vec4);
  if(buf.byteSize()<size) {
    Resources::recycle(std::move(buf));
    buf = device.ssbo(BufferHeap::Upload, Tempest::Uninitialized, std::max<size_t>(size, 4096*sizeof(Vec4)));
    }
  buf.update(vsmUpload.data(), 0, size);
  vsmLast.spheres = &buf;
  vsmLast.count   = uint32_t(vsmUpload.size());
  }

VisualObjects::VsmChanges VisualObjects::vsmChanges() const {
  return vsmLast;
  }

void VisualObjects::preFrameUpdate() {
  preFrameUpdateWind();
  preFrameUpdateMorph();
  }

void VisualObjects::preFrameUpdateWind() {
  for(auto id:objectsWind) {
    auto& object = objects[id];
    auto position = WorldWind::shaderMatrix(object.pos, scene.tickCount, scene.windPeriod,
                                            scene.zWindEnabled, object.wind, object.windIntensity,
                                            scene.windScale, scene.legacyWind);
    updateInstance(id, &position);
    }
  }

void VisualObjects::preFrameUpdateMorph() {
  for(auto it=objectsMorph.begin(); it!=objectsMorph.end(); ) {
    auto& obj = objects[*it];

    bool reschedule = false;
    MorphData data = {};
    for(size_t i=0; i<Resources::MAX_MORPH_LAYERS; ++i) {
      auto&    ani  = obj.morphAnim[i];
      auto&    bk   = *obj.bucketId;
      auto&    anim = (*bk.staticMesh->morph.anim)[ani.id];
      uint64_t time = (scene.tickCount-ani.timeStart);

      float    alpha     = float(time%anim.tickPerFrame)/float(anim.tickPerFrame);
      float    intensity = ani.intensity*morphWeight(obj,ani);

      if(scene.tickCount>ani.timeUntil) {
        data.morph[i].intensity = 0;
        continue;
        }

      // a still face (S_ANGRY) is watched too while it fades in, or has an end time to go at
      const bool fadingIn = !ani.manual && anim.blendIn>0 && float(time)*anim.blendIn<1;
      if(anim.numFrames>1 || ani.manual || ani.timeUntil!=uint64_t(-1) || fadingIn)
        reschedule = true;

      const uint32_t samplesPerFrame = uint32_t(anim.samplesPerFrame);
      if(ani.manual) {
        data.morph[i].indexOffset = uint32_t(anim.index);
        data.morph[i].sample0     = ani.frame0*samplesPerFrame;
        data.morph[i].sample1     = ani.frame1*samplesPerFrame;
        data.morph[i].alpha       = uint16_t(ani.alpha*uint16_t(-1));
        data.morph[i].intensity   = uint16_t(intensity*uint16_t(-1));
        continue;
        }
      data.morph[i].indexOffset = uint32_t(anim.index);
      data.morph[i].sample0     = uint32_t((time/anim.tickPerFrame+0)%anim.numFrames)*samplesPerFrame;
      data.morph[i].sample1     = uint32_t((time/anim.tickPerFrame+1)%anim.numFrames)*samplesPerFrame;
      data.morph[i].alpha       = uint16_t(alpha*uint16_t(-1));
      data.morph[i].intensity   = uint16_t(intensity*uint16_t(-1));
      }

    obj.objMorphAnim.set(&data, 0, sizeof(data));
    if(!reschedule)
      it = objectsMorph.erase(it); else
      it++;
    }
  }

void VisualObjects::visibilityPass(Tempest::Encoder<Tempest::CommandBuffer> &cmd, int pass) {
  drawCmd.visibilityPass(cmd, pass);
  }

void VisualObjects::visibilityVsm(Tempest::Encoder<Tempest::CommandBuffer>& cmd) {
  drawCmd.visibilityVsm(cmd);
  }

void VisualObjects::drawMultiply(Tempest::Encoder<Tempest::CommandBuffer>& cmd) {
  drawCmd.drawCommon(cmd, SceneGlobals::V_Main, Material::Multiply);
  drawCmd.drawCommon(cmd, SceneGlobals::V_Main, Material::Multiply2);
  }

void VisualObjects::drawTranslucent(Tempest::Encoder<Tempest::CommandBuffer>& cmd) {
  // return;
  drawCmd.drawCommon(cmd, SceneGlobals::V_Main, Material::Ghost);
  drawCmd.drawCommon(cmd, SceneGlobals::V_Main, Material::AdditiveLight);
  drawCmd.drawCommon(cmd, SceneGlobals::V_Main, Material::Transparent);
  }

void VisualObjects::drawWater(Tempest::Encoder<Tempest::CommandBuffer>& cmd) {
  // return;
  drawCmd.drawCommon(cmd, SceneGlobals::V_Main, Material::Water);
  }

void VisualObjects::drawGBuffer(Tempest::Encoder<CommandBuffer>& cmd) {
  // return;
  drawCmd.drawCommon(cmd, SceneGlobals::V_Main, Material::Solid);
  drawCmd.drawCommon(cmd, SceneGlobals::V_Main, Material::AlphaTest);
  }

void VisualObjects::drawShadow(Tempest::Encoder<Tempest::CommandBuffer>& cmd, int layer) {
  // return;
  auto view = SceneGlobals::VisCamera(SceneGlobals::V_Shadow0 + layer);
  drawCmd.drawCommon(cmd, view, Material::Solid);
  drawCmd.drawCommon(cmd, view, Material::AlphaTest);
  }

void VisualObjects::drawVsm(Tempest::Encoder<Tempest::CommandBuffer>& cmd, DrawCommands::VsmDraw draw) {
  drawCmd.drawVsm(cmd, draw);
  }

void VisualObjects::drawHiZ(Tempest::Encoder<Tempest::CommandBuffer>& cmd) {
  drawCmd.drawHiZ(cmd);
  }

void VisualObjects::notifyTlas(const Material& m, RtScene::Category cat) {
  scene.rtScene.notifyTlas(m,cat);
  }

void VisualObjects::postFrameupdate() {
  instanceMem.join();
  }

bool VisualObjects::updateRtScene(RtScene& out) {
  if(!out.isUpdateRequired())
    return false;
  for(auto& obj:objects) {
    if(obj.isEmpty() || !obj.visible)
      continue;
    auto& bucket = *obj.bucketId;
    auto* mesh   = bucket.staticMesh;
    auto& mat    = bucket.mat;
    if(mesh==nullptr)
      continue;
    if(auto blas = mesh->blas(obj.iboOff, obj.iboLen)) {
      out.addInstance(obj.pos, *blas, mat, *mesh, obj.iboOff, obj.iboLen, toRtCategory(obj.type));
      }
    }
  out.buildTlas();
  return true;
  }

void VisualObjects::dbgClusters(Tempest::Painter& p, Vec2 wsz) {
  auto cam = Gothic::inst().camera();
  if(cam==nullptr)
    return;
  /*
  for(auto& c:clusters) {
    dbgDraw(p, wsz, *cam, c);
    }
  */

  /*
  if(auto pl = Gothic::inst().player()) {
    Cluster cx;
    cx.pos = pl->position();
    cx.r   = 100;
    dbgDraw(p, wsz, *cam, cx);
    }
  */
  }

void VisualObjects::dbgDraw(Tempest::Painter& p, Vec2 wsz, const Camera& cam, const DrawClusters::Cluster& cx) {
  auto  c       = cx.pos;
  auto  project = cam.projective();

  cam.view().project(c);
  //const vec3  c     = (scene.view * vec4(sphere.xyz, 1)).xyz;
  const float R     = cx.r;
  const float znear = cam.zNear();
  if(c.z - R < znear)
    ;//return;

  // depthMin = znear / (c.z + r);
  // float z = c.z + R;
  // depthMin  = scene.project[3][2]/z + scene.project[2][2];

  float P00 = project[0][0];
  float P11 = project[1][1];

  Vec3  cr   = c * R;
  float czr2 = c.z * c.z - R * R;

  float vx   = std::sqrt(c.x * c.x + czr2);
  float minx = (vx * c.x - cr.z) / (vx * c.z + cr.x);
  float maxx = (vx * c.x + cr.z) / (vx * c.z - cr.x);

  float vy   = std::sqrt(c.y * c.y + czr2);
  float miny = (vy * c.y - cr.z) / (vy * c.z + cr.y);
  float maxy = (vy * c.y + cr.z) / (vy * c.z - cr.y);

  Vec4 aabb;
  aabb = Vec4(minx * P00, miny * P11, maxx * P00, maxy * P11);
  aabb = aabb*0.5 + Vec4(0.5);

  aabb.x = aabb.x * wsz.x;
  aabb.z = aabb.z * wsz.x;

  aabb.y = aabb.y * wsz.y;
  aabb.w = aabb.w * wsz.y;

  if(aabb.x>=aabb.z)
    Log::d("");
  if(aabb.y>=aabb.w)
    Log::d("");

  p.setBrush(Color(0,0,1,0.1f));
  p.drawRect(int(aabb.x), int(aabb.y), int(aabb.z-aabb.x), int(aabb.w-aabb.y));
  }

void VisualObjects::dbgDrawBBox(Tempest::Painter& p, Tempest::Vec2 wsz, const Camera& cam, const DrawClusters::Cluster& c) {
  /*
  auto& b       = buckets[c.bucketId];
  auto  project = cam.viewProj();

  Vec4  aabb     = Vec4(1, 1, -1, -1);
  float depthMin = 1;
  for(uint32_t i=0; i<8; ++i) {
    float x = b[i&0x1 ? 1 : 0].x;
    float y = b[i&0x2 ? 1 : 0].y;
    float z = b[i&0x4 ? 1 : 0].z;

    const Vec3 pos = Vec3(x, y, z);
    Vec4 trPos = Vec4(pos,1.0);
    trPos = Vec4(obj.mat*trPos, 1.0);
    trPos = scene.viewProject*trPos;
    if(trPos.w<znear || false) {
      depthMin = 0;
      aabb     = vec4(0,0,1,1);
      return true;
      }

    vec3 bp = trPos.xyz/trPos.w;

    aabb.xy  = min(aabb.xy,  bp.xy);
    aabb.zw  = max(aabb.zw,  bp.xy);
    depthMin = min(depthMin, bp.z);
    }
  aabb = aabb*0.5 + Vec4(0.5);

  aabb.x = aabb.x * wsz.x;
  aabb.z = aabb.z * wsz.x;

  aabb.y = aabb.y * wsz.y;
  aabb.w = aabb.w * wsz.y;

  if(aabb.x>=aabb.z)
    Log::d("");
  if(aabb.y>=aabb.w)
    Log::d("");

  p.setBrush(Color(0,0,1,0.1f));
  p.drawRect(int(aabb.x), int(aabb.y), int(aabb.z-aabb.x), int(aabb.w-aabb.y));
  */
  }


#include "pfxobjects.h"

#include <Tempest/Log>
#include <cstring>

#include "graphics/drawcommands.h"
#include "graphics/sceneglobals.h"

#include "pfxbucket.h"
#include "particlefx.h"

using namespace Tempest;

PfxObjects::PfxObjects(WorldView& world, const SceneGlobals& scene, VisualObjects& visual)
  :world(world), scene(scene), visual(visual) {
  }

PfxObjects::~PfxObjects() {
  // the landings' emitters live in the buckets
  landings.clear();
  // Buckets release demand through their ParticleFx declarations.
  bucket.clear();
  }

// `view hide pfx` of the test bench (DrawCommands::hidePfxObjects, debug only; not `view clean` or `nopfx`): the
// particles and sprites go through these buckets, not the draw commands; the frames without them (gi-clips.bench,
// X-bare) compare with the path tracer, whose ray scene has none
static bool hiddenByBench() {
  return DrawCommands::hidePfxObjects;
  }

void PfxObjects::setViewerPos(const Vec3& pos) {
  viewerPos = pos;
  }

void PfxObjects::setWind(const Vec3& w) {
  wind = w;
  }

void PfxObjects::setShelter(std::function<float(const Vec3&,const Vec3&)> open) {
  shelter = std::move(open);
  }

void PfxObjects::setGround(std::function<bool(const Vec3&,const Vec3&,Vec3&)> ray) {
  ground = std::move(ray);
  }

void PfxObjects::resetTicks() {
  lastUpdate = size_t(-1);
  }

void PfxObjects::tick(uint64_t ticks) {
  static bool disabled = false;
  if(disabled)
    return;

  if(lastUpdate>ticks) {
    lastUpdate = ticks;
    return;
    }

  uint64_t dt = ticks-lastUpdate;
  if(dt==0)
    return;
  now = ticks;

  for(auto& i:bucket)
    i.tick(dt,viewerPos);
  putLandings();

  for(auto& i:bucket)
    i.buildSsbo();

  lastUpdate = ticks;
  }

void PfxObjects::putLandings() {
  // emitters that have emitted go; their particles live out in their buckets (S_Fade)
  for(size_t i=0; i<landings.size();) {
    if(landings[i].until<=now) {
      landings[i] = std::move(landings.back());
      landings.pop_back();
      } else {
      ++i;
      }
    }
  if(landAsked.empty())
    return;
  // a burst of a hundred drops at once is a hundred effects at most: the rain of a spell, not a downpour
  static constexpr size_t MaxLandings = 256;
  std::lock_guard<std::recursive_mutex> guard(sync);
  for(auto& [decl,at]:landAsked) {
    auto put = [&](const ParticleFx* fx) {
      if(fx==nullptr || landings.size()>=MaxLandings)
        return;
      Landing l;
      l.slot  = PfxSlot(*this,fx);
      l.until = now + std::max<uint64_t>(fx->prefferedTime,100);
      l.slot.setPosition(at);
      l.slot.setLooped(false);
      l.slot.setActive(true);
      landings.push_back(std::move(l));
      };
    put(decl);
    for(auto* other:decl->with)
      put(other);
    }
  landAsked.clear();
  }

bool PfxObjects::isInPfxRange(const Vec3& pos) const {
  auto dp = viewerPos-pos;
  return dp.quadLength()<viewRage*viewRage;
  }

void PfxObjects::preFrameUpdate(uint8_t fId) {
  for(auto i=bucket.begin(), end = bucket.end(); i!=end; ) {
    if(i->isEmpty()) {
      i = bucket.erase(i);
      } else {
      ++i;
      }
    }

  for(auto& i:bucket)
    i.preFrameUpdate(scene, fId);

  // the published list of the frame: the material and its texture handles by value, as DrawBuckets::BucketPub
  pubBucket.clear();
  pubBucket.reserve(bucket.size());
  for(auto& i:bucket) {
    PubBucket pb;
    pb.b       = &i;
    pb.d.mat   = i.decl.visMaterial;
    if(i.decl.visMaterial.tex!=nullptr)
      pb.d.tex = i.decl.visMaterial.tex->shared();
    pb.d.frames.reserve(i.decl.visMaterial.frames.size());
    for(const auto* t:i.decl.visMaterial.frames)
      pb.d.frames.push_back(t->shared());
    if(i.decl.trlTexture!=nullptr)
      pb.d.trl = i.decl.trlTexture->shared();
    pb.d.hazeShift = i.decl.hazeShift;
    pubBucket.push_back(std::move(pb));
    }
  }

void PfxObjects::drawGBuffer(Tempest::Encoder<Tempest::CommandBuffer>& cmd, uint8_t fId, bool historyOwner) {
  if(hiddenByBench())
    return;
  for(const auto& i:pubBucket)
    i.b->drawGBuffer(cmd, scene, fId, historyOwner, i.d);
  }

void PfxObjects::drawShadow(Tempest::Encoder<Tempest::CommandBuffer>& cmd, uint8_t fId, int layer) {
  if(hiddenByBench())
    return;
  for(const auto& i:pubBucket)
    i.b->drawShadow(cmd, scene, fId, layer, i.d);
  }

void PfxObjects::drawMultiply(Tempest::Encoder<Tempest::CommandBuffer>& cmd, uint8_t fId) {
  if(hiddenByBench())
    return;
  for(const auto& i:pubBucket)
    i.b->drawMultiply(cmd, scene, fId, i.d);
  }

void PfxObjects::drawTranslucent(Tempest::Encoder<Tempest::CommandBuffer>& cmd, uint8_t fId) {
  if(hiddenByBench())
    return;
  for(const auto& i:pubBucket)
    i.b->drawTranslucent(cmd, scene, fId, i.d);
  }

bool PfxObjects::hasHaze(uint8_t fId) const {
  if(hiddenByBench())
    return false;
  for(const auto& i:pubBucket)
    if(i.b->hasHaze(fId))
      return true;
  return false;
  }

void PfxObjects::drawHaze(Tempest::Encoder<Tempest::CommandBuffer>& cmd, uint8_t fId) {
  if(hiddenByBench())
    return;
  for(const auto& i:pubBucket)
    i.b->drawHaze(cmd, scene, fId, i.d);
  }

PfxBucket& PfxObjects::getBucket(const ParticleFx &decl) {
  for(auto& i:bucket)
    if(&i.decl==&decl)
      return i;
  bucket.emplace_back(decl,*this,scene,visual);
  return bucket.back();
  }

PfxBucket& PfxObjects::getBucket(const Material& mat, const asset::VirtualObject& vob) {
  Tempest::Vec2 dimension {};
  if(auto decal = dynamic_cast<const asset::VisualDecal*>(vob.visual.get()))
    dimension = Tempest::Vec2{decal->dimension.x, decal->dimension.y};

  for(auto& i:spriteEmit)
    if(i.pfx->visMaterial==mat &&
       i.visualCamAlign==vob.sprite_camera_facing_mode &&
       i.zBias==vob.bias &&
       i.decalDim==dimension) {
      return getBucket(*i.pfx);
      }
  spriteEmit.emplace_back();
  auto& e = spriteEmit.back();

  e.visualCamAlign = vob.sprite_camera_facing_mode;
  e.zBias          = vob.bias;
  e.decalDim       = dimension;
  e.pfx.reset(new ParticleFx(mat,vob));
  return getBucket(*e.pfx);
  }


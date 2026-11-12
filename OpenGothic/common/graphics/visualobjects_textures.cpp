// Texture owners of VisualObjects: the demand of each draw instance on its material's textures, by ring or by its size
// on screen (docs/world-streaming-textures.md, reports/engine/2026-11-10-texture-mips/README.md).
#include "visualobjects.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

#include "frameprofile.h"
#include "world/chunkresidency.h"

using namespace Tempest;

namespace {
constexpr float Unbounded = std::numeric_limits<float>::infinity();

// How far, in centimetres, `position` is from the nearest edge of its chunk in x and z.
float chunkEdge(const Vec3& position) {
  const auto chunk=ChunkId::fromPosition(position.x,position.z);
  const double x=double(position.x)-double(chunk.x)*ChunkId::Size;
  const double z=double(position.z)-double(chunk.z)*ChunkId::Size;
  return float(std::max(0.0,std::min({x,double(ChunkId::Size)-x,z,double(ChunkId::Size)-z})));
  }
}

void VisualObjects::Item::setTexturePosition(const Vec3& position) {
  if(owner==nullptr)
    return;
  auto& obj=owner->objects[id];
  if(obj.textureCategory!=Resources::TextureCategory::Static)
    return;
  const auto chunk=ChunkId::fromPosition(position.x,position.z);
  // With levels by screen size a measured owner is placed again when it moves half a metre or turns or scales by about
  // three degrees or five per cent: its bounds decide.
  const auto dx=position.x-obj.textureAnchor.x, dy=position.y-obj.textureAnchor.y, dz=position.z-obj.textureAnchor.z;
  const auto turned=[&obj] {
    float diff=0, norm=0;
    for(int i=0;i<3;++i)
      for(int k=0;k<3;++k) {
        const float d=obj.pos.at(i,k)-obj.textureAxes[i][k];
        diff+=d*d;
        norm+=obj.textureAxes[i][k]*obj.textureAxes[i][k];
        }
    return diff>0.0025f*norm/3.f;
    };
  if(obj.textureChunkRegistered && obj.textureChunk==std::pair{chunk.x,chunk.z} &&
     (!obj.textureMeasured || !owner->textureScreenMode ||
      (obj.texturePlaced && dx*dx+dy*dy+dz*dz<50.f*50.f && !turned())))
    return;
  FrameProfile::Scope timing(FrameProfile::Zone::TextureDemand);
  std::lock_guard<std::recursive_mutex> guard(owner->textureSync);
  ++owner->textureCensus.placements;
  obj.texturePosition=position;
  owner->placeTexture(obj);
  owner->updateTextureDemand(id);
  }

float VisualObjects::Item::setTextureOwner(const Vec3& position, bool spatial) {
  if(owner==nullptr)
    return Unbounded;
  auto& obj=owner->objects[id];
  const bool same=obj.textureCategory==Resources::TextureCategory::Dynamic && obj.textureWorldOwner==spatial;
  if(same && !spatial)
    return Unbounded;
  // A spatial owner still within its reach of where it was last evaluated: one comparison, no lock. Only this
  // owner's own calls write the anchor and the reach (below), so the pair read here is one it set itself.
  if(same && obj.textureChunkRegistered) {
    const float dx=position.x-obj.texturePosition.x, dy=position.y-obj.texturePosition.y, dz=position.z-obj.texturePosition.z;
    const float moved=dx*dx+dy*dy+dz*dz;
    if(moved<=obj.textureReach*obj.textureReach)
      return obj.textureReach-std::sqrt(moved);
    }
  FrameProfile::Scope timing(FrameProfile::Zone::TextureDemand);
  std::lock_guard<std::recursive_mutex> guard(owner->textureSync);
  if(same) {
    ++owner->textureCensus.moves;
    } else {
    owner->setTextureDemand(obj,false);
    if(obj.textureChunkRegistered) {
      if(auto found=owner->textureChunks.find(obj.textureChunk);found!=owner->textureChunks.end()) {
        found->second.ids.erase(id);
        if(found->second.ids.empty())
          owner->textureChunks.erase(found);
        }
      obj.textureChunkRegistered=false;
      }
    obj.textureCategory=Resources::TextureCategory::Dynamic;
    obj.textureChunkedLandscape=false;
    obj.textureWorldOwner=spatial;
    }
  obj.texturePosition=position;
  owner->placeTexture(obj);
  // The allowance is fixed here, at the owner's own evaluation, and holds until the next one; every evaluation until
  // then, the view queue's too, measures to its sphere grown by it (evaluateTexture).
  const float reach=owner->textureAllowance(obj,same);
  obj.textureReach=reach;
  owner->updateTextureDemand(id);
  return reach;
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
      found->second.ids.erase(id);
      if(found->second.ids.empty())
        owner->textureChunks.erase(found);
      }
    obj.textureChunkRegistered=false;
    }
  if(obj.textureWorldOwner) {
    // Its sphere was placed by a position: a static owner is placed again by setTexturePosition.
    obj.texturePlaced=false;
    obj.textureRadius=-1;
    }
  obj.textureCategory=category;
  obj.textureWorldOwner=false;
  if(category!=Resources::TextureCategory::Landscape)
    obj.textureChunkedLandscape=false;
  owner->updateTextureDemand(id);
  }

void VisualObjects::placeTexture(Object& object) {
  if(!object.textureMeasured || object.textureChunkedLandscape)
    return;
  if(object.textureWorldOwner) {
    // A sphere around the registered position with the mesh's radius from its origin, the density at the object's
    // scale: neither depends on its rotation or pose.
    const float scale=TexelDensity::scaleOf(object.pos);
    const float s=scale>0 ? scale : 1.f;
    object.textureTexel=TexelDensity();
    object.textureTexel.flat=object.textureLocal.flat;
    if(object.textureLocal.valid()) {
      object.textureTexel.uvPerMetre=object.textureLocal.uvPerMetre/s;
      object.textureTexel.min=object.texturePosition;
      object.textureTexel.max=object.texturePosition;
      }
    object.textureRadius=object.textureLocal.radius()*s;
    object.textureScale=s;
    } else {
    object.textureTexel=object.textureLocal.placed(object.pos);
    object.textureRadius=-1;
    for(int i=0;i<3;++i)
      for(int k=0;k<3;++k)
        object.textureAxes[i][k]=object.pos.at(i,k);
    }
  object.textureAnchor=object.texturePosition;
  object.texturePlaced=true;
  }

void VisualObjects::updateTextureScale(size_t id) {
  auto& object=objects[id];
  if(!object.textureWorldOwner || !object.texturePlaced || !object.textureMeasured || !textureScreenMode)
    return;
  float axis=0;
  for(int i=0;i<3;++i) {
    const float x=object.pos.at(i,0), y=object.pos.at(i,1), z=object.pos.at(i,2);
    axis=std::max(axis,x*x+y*y+z*z);
    }
  // as placeTexture: no scale counts as 1
  const float now=axis>0 && std::isfinite(axis) ? axis : 1.f;
  const float was=object.textureScale*object.textureScale;
  if(now>0.9025f*was && now<1.1025f*was)
    return;
  FrameProfile::Scope timing(FrameProfile::Zone::TextureDemand);
  std::lock_guard<std::recursive_mutex> guard(textureSync);
  ++textureCensus.placements;
  placeTexture(object);
  updateTextureDemand(id);
  }

TextureScreenDemand::Result VisualObjects::evaluateTexture(const Object& object) const {
  if(object.textureRadius>=0) {
    // The owner may be anywhere within its reach of the anchor: the sphere grows by it, the view gets the whole margin.
    const float grown=object.textureRadius+(std::isfinite(object.textureReach) ? object.textureReach : 0.f);
    return TextureScreenDemand::evaluate(textureView,object.textureTexel,{object.texturePosition,grown},
                                         object.textureNeed.resolution,textureClass(object),object.textureNeed.present);
    }
  return TextureScreenDemand::evaluate(textureView,object.textureTexel,object.textureNeed.resolution,textureClass(object),
                                       object.textureNeed.present);
  }

uint32_t VisualObjects::textureClass(const Object& object) {
  // An owner registered with an exponent holds its distance class in the ring; before that, the ring is its chunk's.
  return object.textureNeed.resolution!=TextureLevels::NoExponent ? object.textureNeed.ring : TextureScreenDemand::NoClass;
  }

float VisualObjects::textureAllowance(const Object& object, bool moved) const {
  if(!object.textureWorldOwner)
    return Unbounded;
  // Within the edge of its chunk in both rules: its ring stays that of its chunk, also when the rule is switched.
  const float edge=chunkEdge(object.texturePosition);
  if(!textureScreenOwner(object) || !object.texturePlaced)
    return edge;
  // A newly placed owner gets none: one that never moves (an item lying, a mobsi) is evaluated exactly. One that
  // moved gets half the room its exact distance has to the nearer edge of its exponent's band, so the view keeps at
  // least the other half, and no more than MaxReach: the sphere errs sharper by at most that.
  if(!moved)
    return 0;
  const auto exact=TextureScreenDemand::evaluate(textureView,object.textureTexel,{object.texturePosition,object.textureRadius},
                                                 object.textureNeed.resolution,textureClass(object));
  if(!exact.scheduled)
    return edge;  // the whole chain or nothing to draw: no distance
  if(!(exact.margin>0))
    return 0;     // no view yet
  return std::min({float(exact.margin*50.0),TextureScreenDemand::MaxReach,edge});
  }

double VisualObjects::textureDistance(const Object& object) const {
  const auto& t=object.textureTexel;
  if(!t.valid() || !textureView.known)
    return -1;
  if(object.textureRadius>=0) {
    const TextureScreenDemand::Sphere sphere{object.texturePosition,object.textureRadius};
    return std::min(TextureScreenDemand::distance(textureView.hero,sphere),TextureScreenDemand::distance(textureView.camera,sphere));
    }
  return std::min(TextureScreenDemand::distance(textureView.hero,t),TextureScreenDemand::distance(textureView.camera,t));
  }

// The draw instances' demand goes with them; the rest of the object is plain data.
VisualObjects::~VisualObjects() {
  std::lock_guard<std::recursive_mutex> guard(textureSync);
  if(textureResidency) {
    textureResidency->unsubscribe(textureSubscription);
    textureResidency->unsubscribe(textureFocusSubscription);
    textureResidency->unsubscribe(textureViewSubscription);
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
    textureResidency->unsubscribe(textureViewSubscription);
    }
  textureResidency=residency;
  textureScreenMode=Resources::textureScreenMips();
  textureView=TextureScreenDemand::View();
  if(textureResidency) {
    textureSubscription=textureResidency->subscribe([this](ChunkId chunk, ChunkResidency::State, ChunkResidency::State) {
      FrameProfile::Scope timing(FrameProfile::Zone::TextureDemand);
      std::lock_guard<std::recursive_mutex> guard(textureSync);
      const auto found=textureChunks.find({chunk.x,chunk.z});
      if(found==textureChunks.end())
        return;
      for(auto id:found->second.ids)
        updateTextureDemand(id);
      });
    textureFocusSubscription=textureResidency->subscribeFocus([this] { updateTextureFocus(); });
    textureViewSubscription=textureResidency->subscribeView([this] { updateTextureView(); });
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
  Resources::textureDemand(object.bucketId->mat,object.textureCategory,active,object.textureNeed);
  object.textureDemand=active;
  }

void VisualObjects::scheduleTexture(Object& object, size_t id, double expiry) {
  unscheduleTexture(object);
  object.textureScheduled=true;
  object.textureStamp=++textureStampNext;
  ++textureScheduledCount;
  textureScreen.push(id,expiry,object.textureStamp);
  }

void VisualObjects::unscheduleTexture(Object& object) {
  // its entry stays in the heap and is dropped when it comes up
  if(object.textureScheduled)
    --textureScheduledCount;
  object.textureScheduled=false;
  }

bool VisualObjects::textureScreenOwner(const Object& object) const {
  if(!textureScreenMode || !object.textureMeasured)
    return false;
  // Landscape packages and static objects, and since step 2 the owners placed by a position (NPCs, items, mobsi).
  return object.textureChunkedLandscape || object.textureWorldOwner ||
         object.textureCategory==Resources::TextureCategory::Static;
  }

void VisualObjects::updateTextureDemand(size_t id) {
  std::lock_guard<std::recursive_mutex> guard(textureSync);
  ++textureCensus.demandCalls;
  auto& object=objects[id];
  if(object.isEmpty())
    return;
  unscheduleTexture(object);
  if(object.textureCategory!=Resources::TextureCategory::Static && !object.textureChunkedLandscape && !object.textureWorldOwner) {
    if(object.textureNeed!=Resources::TextureNeed()) {
      setTextureDemand(object,false);
      object.textureNeed=Resources::TextureNeed();
      }
    setTextureDemand(object,true);
    return;
    }
  const auto chunk=ChunkId::fromPosition(object.texturePosition.x,object.texturePosition.z);
  const std::pair key{chunk.x,chunk.z};
  if(!object.textureChunkRegistered || key!=object.textureChunk) {
    if(object.textureChunkRegistered)
      if(auto found=textureChunks.find(object.textureChunk);found!=textureChunks.end()) {
        found->second.ids.erase(id);
        if(found->second.ids.empty())
          textureChunks.erase(found);
        }
    object.textureChunk=key;
    textureChunks[key].ids.insert(id);
    object.textureChunkRegistered=true;
    }
  Resources::TextureNeed need;
  need.ring=textureResidency ? std::min(4u,textureResidency->textureDistance(chunk)) : 0;
  need.resolution=TextureLevels::NoExponent;
  // Measured on screen once placed; an unplaced static one keeps the ring rule until it is. An owner placed by a
  // position is measured with its reach (textureAllowance), which only its own setTextureOwner changes.
  if(textureScreenOwner(object) && object.texturePlaced) {
    const auto demand=evaluateTexture(object);
    need.resolution=demand.exponent;
    need.present=demand.present;
    // measured from a distance: the stages coarsen by its distance class, not by the ring of its chunk
    if(demand.distanceClass!=TextureScreenDemand::NoClass)
      need.ring=demand.distanceClass;
    if(demand.scheduled)
      scheduleTexture(object,id,demand.expiry);
    }
  // A landscape owner exists only while its package is published.
  const bool active=object.textureWorldOwner || object.textureChunkedLandscape || textureChunkActive(object.texturePosition);
  if(object.textureNeed!=need) {
    if(object.textureDemand && active)
      Resources::textureDemand(object.bucketId->mat,object.textureCategory,object.textureNeed,need);
    else
      setTextureDemand(object,false);
    object.textureNeed=need;
    }
  setTextureDemand(object,active);
  }

void VisualObjects::updateTextureFocus() {
  FrameProfile::Scope timing(FrameProfile::Zone::TextureDemand);
  std::lock_guard<std::recursive_mutex> guard(textureSync);
  // The focus listeners run before the view ones: owners evaluated here see this update's view.
  textureView.hero     = textureResidency->viewHero();
  textureView.camera   = textureResidency->viewCamera();
  textureView.odometer = textureResidency->viewOdometer();
  textureView.known    = textureResidency->viewKnown();
  textureView.coarsest = TextureLevels::coarsest(Resources::textureMipReserve(),Resources::textureViewScaleLog2());
  textureView.heroAhead   = textureResidency->viewHeroAhead();
  textureView.cameraAhead = textureResidency->viewCameraAhead();
  const auto started=textureCensus.enabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
  for(auto& entry:textureChunks) {
    const ChunkId chunk{entry.first.first,entry.first.second};
    const auto distance=std::min(4u,textureResidency->textureDistance(chunk));
    if(entry.second.ring==distance)
      continue;
    entry.second.ring=distance;
    for(auto id:entry.second.ids) {
      const auto& object=objects[id];
      // A measured owner follows its distance (updateTextureView), not the ring: a focus change costs it nothing.
      if(object.textureNeed.ring==distance || (textureScreenOwner(object) && object.texturePlaced))
        continue;
      ++textureCensus.focusOwners;
      updateTextureDemand(id);
      }
    }
  if(textureCensus.enabled) {
    const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
    textureCensus.focusMs+=ms;
    textureCensus.focusMaxMs=std::max(textureCensus.focusMaxMs,ms);
    }
  }

void VisualObjects::updateTextureView(bool flush) {
  std::lock_guard<std::recursive_mutex> guard(textureSync);
  if(textureResidency==nullptr)
    return;
  textureView.hero     = textureResidency->viewHero();
  textureView.camera   = textureResidency->viewCamera();
  textureView.odometer = textureResidency->viewOdometer();
  textureView.known    = textureResidency->viewKnown();
  textureView.coarsest = TextureLevels::coarsest(Resources::textureMipReserve(),Resources::textureViewScaleLog2());
  textureView.heroAhead   = textureResidency->viewHeroAhead();
  textureView.cameraAhead = textureResidency->viewCameraAhead();
  // The distance class adds its margin from stage 2 on, where the stages use it. Entering such a stage, every owner
  // is evaluated once after the due ones, within the same budget: its class may have gone stale meanwhile.
  const bool classMargin=Resources::textureStage()>=2;
  if(classMargin!=textureView.classMargin)
    textureClassSweep=classMargin ? 0 : size_t(-1);
  textureView.classMargin=classMargin;
  if(textureCensus.enabled)
    reportTextureCensus();
  const auto started=textureCensus.enabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
  if(textureScreenMode!=Resources::textureScreenMips()) {
    // The rule changed with the settings: every owner registers again, measured ones placed where they are.
    FrameProfile::Scope timing(FrameProfile::Zone::TextureDemand);
    textureScreenMode=Resources::textureScreenMips();
    for(size_t id=0;id<objects.size();++id) {
      auto& object=objects[id];
      if(object.isEmpty())
        continue;
      if(textureScreenMode)
        placeTexture(object);
      updateTextureDemand(id);
      }
    return;
    }
  TextureScreenDemand::Entry entry;
  const bool sweeping=textureView.known && textureClassSweep<objects.size();
  if(!textureView.known || (!textureScreen.due(textureView.odometer,entry) && !sweeping))
    return;
  // Owners due: a quarter of a millisecond a frame; a teleport catches up over a few frames (the focus change has
  // already evaluated the owners within ring 4). An unchanged exponent is only scheduled again.
  FrameProfile::Scope timing(FrameProfile::Zone::TextureDemand);
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::microseconds(250);
  const auto evaluate=[&](size_t id, Object& object) {
    ++textureCensus.evaluations;
    const auto demand=evaluateTexture(object);
    if(!demand.scheduled) {
      updateTextureDemand(id);
      return;
      }
    // Only the exponents and the distance class can have changed here; a measured owner's ring is its class.
    if(demand.exponent!=object.textureNeed.resolution || demand.distanceClass!=object.textureNeed.ring ||
       demand.present!=object.textureNeed.present) {
      if(demand.exponent==object.textureNeed.resolution && demand.present==object.textureNeed.present)
        ++textureCensus.classes;
      auto need=object.textureNeed;
      need.resolution=demand.exponent;
      need.present=demand.present;
      need.ring=demand.distanceClass;
      if(object.textureDemand)
        Resources::textureDemand(object.bucketId->mat,object.textureCategory,object.textureNeed,need);
      object.textureNeed=need;
      }
    scheduleTexture(object,id,demand.expiry);
    };
  bool late=false;
  for(size_t count=1;textureScreen.due(textureView.odometer,entry);++count) {
    textureScreen.pop();
    // free() pops the empty tail of `objects`: an entry of a released owner can be past its end
    if(entry.owner>=objects.size()) {
      ++textureDroppedPastEnd[0];
      continue;
      }
    auto& object=objects[entry.owner];
    if(object.isEmpty() || !object.textureScheduled || object.textureStamp!=entry.stamp) {
      ++textureCensus.stale;
      continue;
      }
    evaluate(entry.owner,object);
    if(!flush && count%16==0 && std::chrono::steady_clock::now()>=deadline) {
      late=true;
      ++textureCensus.late;
      if(textureScreen.due(textureView.odometer,entry))
        textureCensus.lateMetres=std::max(textureCensus.lateMetres,textureView.odometer-entry.expiry);
      break;
      }
    }
  // The sweep after a switch to a stage that uses the class: what the due owners left of the budget, also on a flush.
  for(size_t count=1;!late && textureClassSweep<objects.size();++count) {
    const size_t id=textureClassSweep++;
    auto& object=objects[id];
    if(!object.isEmpty() && object.textureScheduled) {
      ++textureCensus.sweep;
      evaluate(id,object);
      }
    if(count%16==0 && std::chrono::steady_clock::now()>=deadline)
      break;
    }
  if(textureCensus.enabled) {
    const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
    textureCensus.viewMs+=ms;
    textureCensus.viewMaxMs=std::max(textureCensus.viewMaxMs,ms);
    }
  if(textureScreen.size()>2*textureScheduledCount+4096) {
    ++textureCensus.compactions;
    textureScreen.compact([this](const TextureScreenDemand::Entry& e) {
      if(e.owner>=objects.size()) {
        ++textureDroppedPastEnd[1];
        return false;
        }
      const auto& object=objects[e.owner];
      return !object.isEmpty() && object.textureScheduled && object.textureStamp==e.stamp;
      });
    }
  }


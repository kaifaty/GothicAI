#include "pfxbucket.h"

#include <algorithm>
#include <cassert>
#include <cmath>

#include "frameprofile.h"
#include "graphics/mesh/submesh/pfxemittermesh.h"
#include "graphics/shaders.h"
#include "graphics/visualobjects.h"
#include "graphics/drawcommands.h"
#include "graphics/worldview.h"
#include "pfxobjects.h"
#include "particlefx.h"
#include "world/objects/npc.h"
#include "world/chunkresidency.h"

#include <Tempest/Log>

using namespace Tempest;

static uint64_t ppsDiff(const ParticleFx& decl, bool loop, uint64_t time0, uint64_t time1) {
  if(time1<=time0)
    return 0;
  if(decl.prefferedTime==0 && time0==0 && !loop)
    return uint64_t(decl.ppsValue*1000.f);
  if(!loop) {
    time0 = std::min(decl.prefferedTime,time0);
    time1 = std::min(decl.prefferedTime,time1);
    }
  const float pps     = decl.ppsScale(time1)*decl.ppsValue;
  uint64_t    emitted0 = uint64_t(pps*float(time0)/1000.f);
  uint64_t    emitted1 = uint64_t(pps*float(time1)/1000.f);
  return emitted1-emitted0;
  }

float PfxBucket::ParState::lifeTime() const {
  return 1.f-life/float(maxLife);
  }

std::mt19937 PfxBucket::rndEngine;

bool PfxBucket::Draw::isEmpty() const {
  return (pfxGpu.byteSize()==0);
  }

PfxBucket::PfxBucket(const ParticleFx &decl, PfxObjects& parent, const SceneGlobals& scene, VisualObjects& visual)
  :decl(decl), parent(parent), visual(visual)  {
  uint64_t lt      = decl.maxLifetime();
  uint64_t pps     = uint64_t(std::ceil(decl.maxPps()));
  uint64_t reserve = (lt*pps+1000-1)/1000;
  blockSize        = decl.flipSteady ? size_t(std::ceil(decl.ppsValue)) : size_t(reserve);
  if(blockSize==0)
    blockSize=1;

  for(size_t i=0; i<Resources::MaxFramesInFlight; ++i) {
    auto& item = this->item[i];
    if(decl.visMaterial.tex==nullptr)
      continue;

    item.pMain   = Shaders::inst().materialPipeline(decl.visMaterial, DrawCommands::Pfx, Shaders::T_Main, false);
    item.pShadow = Shaders::inst().materialPipeline(decl.visMaterial, DrawCommands::Pfx, Shaders::T_Shadow, false);
    }

  if(decl.hasTrails()) {
    maxTrlTime = uint64_t(decl.trlFadeSpeed*1000.f);

    Material mat = decl.visMaterial;
    mat.tex = decl.trlTexture;

    for(size_t i=0; i<Resources::MaxFramesInFlight; ++i) {
      auto& item = this->itemTrl[i];
      if(mat.tex==nullptr)
        continue;

      item.pMain   = Shaders::inst().materialPipeline(mat, DrawCommands::Pfx, Shaders::T_Main, false);
      item.pShadow = Shaders::inst().materialPipeline(mat, DrawCommands::Pfx, Shaders::T_Shadow, false);
      }
    }
  if(decl.isDecal() && (textureResidency=visual.textureResidencyManager())!=nullptr) {
    textureSubscription=textureResidency->subscribe([this](ChunkId chunk, ChunkResidency::State, ChunkResidency::State next) {
      FrameProfile::Scope timing(FrameProfile::Zone::TextureDemand);
      std::lock_guard<std::recursive_mutex> guard(this->parent.sync);
      const auto found=textureChunks.find({chunk.x,chunk.z});
      if(found==textureChunks.end())
        return;
      for(auto id:found->second)
        setTextureDemand(impl[id],next!=ChunkResidency::State::Outside);
      });
    }
  }

PfxBucket::~PfxBucket() {
  std::lock_guard<std::recursive_mutex> guard(parent.sync);
  if(textureResidency)
    textureResidency->unsubscribe(textureSubscription);
  for(auto& emitter:impl)
    setTextureDemand(emitter,false);
  }

void PfxBucket::setTextureDemand(ImplEmitter& emitter, bool active) {
  std::lock_guard<std::recursive_mutex> guard(parent.sync);
  if(emitter.textureDemand==active)
    return;
  const auto category=decl.isDecal() ? Resources::TextureCategory::Static : Resources::TextureCategory::Dynamic;
  Resources::textureDemand(decl.visMaterial,category,active);
  if(decl.hasTrails()) {
    Material trail;
    trail.tex=decl.trlTexture;
    Resources::textureDemand(trail,category,active);
    }
  emitter.textureDemand=active;
  }

void PfxBucket::updateTextureDemand(size_t id) {
  FrameProfile::Scope timing(FrameProfile::Zone::TextureDemand);
  std::lock_guard<std::recursive_mutex> guard(parent.sync);
  auto& emitter=impl[id];
  if(decl.isDecal()) {
    const auto chunk=ChunkId::fromPosition(emitter.pos.x,emitter.pos.z);
    const std::pair key{chunk.x,chunk.z};
    if(emitter.st==S_Free || key!=emitter.textureChunk) {
      if(auto found=textureChunks.find(emitter.textureChunk);found!=textureChunks.end()) {
        found->second.erase(id);
        if(found->second.empty())
          textureChunks.erase(found);
        }
      }
    emitter.textureChunk=key;
    if(emitter.st!=S_Free)
      textureChunks[key].insert(id);
    }
  const bool active=emitter.st!=S_Free && (!decl.isDecal() || visual.textureChunkActive(emitter.pos));
  setTextureDemand(emitter,active);
  }

bool PfxBucket::isEmpty() const {
  for(size_t i=0; i<Resources::MaxFramesInFlight; ++i) {
    if(!item[i].isEmpty())
      return false;
    //if(!itemTrl[i].isEmpty())
    //  return false;
    }
  return impl.size()==0;
  }

void PfxBucket::invalidateDecal() {
  if(!decl.isDecal())
    return;
  for(auto& dirty:forceUpdate)
    dirty = true;
  }

size_t PfxBucket::allocBlock() {
  for(size_t i=0; i<Resources::MaxFramesInFlight; ++i)
    forceUpdate[i] = true;

  for(size_t i=0;i<block.size();++i) {
    if(!block[i].allocated) {
      block[i].allocated = true;
      block[i].timeTotal = 0;
      block[i].flicker   = 1.f;
      return i;
      }
    }

  block.emplace_back();

  Block& b = block.back();
  b.allocated = true;
  b.offset    = particles.size();
  b.timeTotal = 0;

  particles.resize(particles.size()+blockSize);
  pfxCpu   .resize(particles.size());

  for(size_t i=0; i<blockSize; ++i)
    particles[b.offset+i].life = 0;
  return block.size()-1;
  }

void PfxBucket::freeBlock(size_t& i) {
  if(i==size_t(-1))
    return;
  auto& b = block[i];
  assert(b.count==0);

  pfxCpu[b.offset].size = Vec3();

  b.allocated = false;
  i = size_t(-1);
  }

PfxBucket::Block& PfxBucket::getBlock(ImplEmitter &e) {
  if(e.block==size_t(-1)) {
    e.block = allocBlock();
    auto& p = block[e.block];
    p.pos   = e.pos;
    }
  return block[e.block];
  }

PfxBucket::Block& PfxBucket::getBlock(PfxEmitter& e) {
  return getBlock(impl[e.id]);
  }

size_t PfxBucket::allocEmitter() {
  for(size_t i=0; i<impl.size(); ++i) {
    auto& b = impl[i];
    if(b.st==S_Free) {
      b.st      = S_Inactive;
      b.flicker = {};
      updateTextureDemand(i);
      return i;
      }
    }
  impl.emplace_back();
  auto& e = impl.back();
  e.block = size_t(-1); // no backup memory
  e.st    = S_Inactive;
  updateTextureDemand(impl.size()-1);
  for(size_t i=0; i<Resources::MaxFramesInFlight; ++i)
    forceUpdate[i] = true;

  return impl.size()-1;
  }

void PfxBucket::freeEmitter(size_t& id) {
  auto& v = impl[id];
  if(v.block!=size_t(-1)) {
    auto& b = getBlock(v);
    v.st    = b.count==0 ? S_Free : S_Fade;
    if(b.count==0)
      freeBlock(v.block);
    } else {
    v.st    = S_Free;
    }
  for(size_t i=0; i<Resources::MaxFramesInFlight; ++i)
    forceUpdate[i] = true;
  updateTextureDemand(id);
  v.next.reset();
  v.skin = nullptr;  // its owner's palette may go: a free or fading emitter emits no more
  id = size_t(-1);
  shrink();
  }

bool PfxBucket::shrink() {
  while(impl.size()>0) {
    auto& b = impl.back();
    if(b.st!=S_Free)
      break;
    impl.pop_back();
    }
  while(block.size()>0) {
    auto& b = block.back();
    if(b.allocated)
      break;
    block.pop_back();
    }
  if(particles.size()!=block.size()*blockSize) {
    particles.resize(block.size()*blockSize);
    pfxCpu   .resize(particles.size());
    return true;
    }
  return false;
  }

float PfxBucket::randf() {
  return float(rndEngine()%10000)/10000.f;
  }

float PfxBucket::randf(float base, float var) {
  return (2.f*randf()-1.f)*var + base;
  }

void PfxBucket::init(PfxBucket::Block& block, ImplEmitter& emitter, size_t particle) {
  auto& p   = particles[particle];

  if(decl.ppsValue<0) {
    // decal
    p.life    = uint16_t(-1)/2;
    p.maxLife = uint16_t(-1);
    } else if(decl.flipSteady) {
    p.life    = uint16_t(-1);
    p.maxLife = p.life;
    } else {
    p.life    = uint16_t(randf(decl.lspPartAvg,decl.lspPartVar));
    p.maxLife = p.life;
    }

  // TODO: pfx.shpDistribType, pfx.shpDistribWalkSpeed;
  switch(decl.shpType) {
    case ParticleFx::EmitterType::Point:{
      p.pos = Vec3();
      break;
      }
    case ParticleFx::EmitterType::Line:{
      float at = randf();
      p.pos = Vec3(at,at,at);
      break;
      }
    case ParticleFx::EmitterType::Box:{
      if(decl.shpIsVolume) {
        p.pos = Vec3(randf()*2.f-1.f,
                     randf()*2.f-1.f,
                     randf()*2.f-1.f);
        //p.pos*=0.5;
        } else {
        // TODO
        p.pos = Vec3(randf()*2.f-1.f,
                     randf()*2.f-1.f,
                     randf()*2.f-1.f);
        //p.pos*=0.5;
        }
      break;
      }
    case ParticleFx::EmitterType::Sphere:{
      float theta = float(2.0*M_PI)*randf();
      float phi   = std::acos(1.f - 2.f * randf());
      p.pos = Vec3(std::sin(phi) * std::cos(theta),
                   std::sin(phi) * std::sin(theta),
                   std::cos(phi));
      //p.pos*=0.5;
      if(decl.shpIsVolume)
        p.pos*=randf();
      break;
      }
    case ParticleFx::EmitterType::Circle:{
      float a = float(2.0*M_PI)*randf();
      p.pos = Vec3(std::sin(a),
                   0,
                   std::cos(a));
      //p.pos*=0.5;
      if(decl.shpIsVolume)
        p.pos = p.pos*std::sqrt(randf());
      break;
      }
    case ParticleFx::EmitterType::Mesh:{
      p.pos = Vec3();
      auto mesh = (emitter.mesh!=nullptr) ? emitter.mesh : decl.shpMesh;
      auto pose = (emitter.mesh!=nullptr) ? emitter.pose : nullptr;
      auto skin = (emitter.mesh!=nullptr) ? emitter.skin : nullptr;
      if(mesh!=nullptr) {
        auto pos = skin!=nullptr ? mesh->randCoord(randf(),pose,skin->data(),skin->size()) : mesh->randCoord(randf(),pose);
        pos -= emitter.pos;
        p.pos = emitter.direction[0]*pos.x +
                emitter.direction[1]*pos.y +
                emitter.direction[2]*pos.z;
        }
      break;
      }
    }

  if(decl.shpType!=ParticleFx::EmitterType::Point &&
     decl.shpType!=ParticleFx::EmitterType::Mesh) {
    Vec3 dim = decl.shpDim*decl.shpScale(block.timeTotal);
    p.pos.x*=dim.x;
    p.pos.y*=dim.y;
    p.pos.z*=dim.z;
    }

  switch(decl.shpFOR) {
    case ParticleFx::Frame::Object:
    case ParticleFx::Frame::Node: {
      p.pos += emitter.direction[0]*decl.shpOffsetVec.x +
               emitter.direction[1]*decl.shpOffsetVec.y +
               emitter.direction[2]*decl.shpOffsetVec.z;
      break;
      }
    case ParticleFx::Frame::World: {
      p.pos += decl.shpOffsetVec;
      break;
      }
    }

  switch(decl.dirMode) {
    case ParticleFx::Dir::Rand: {
      float dy    = 1.f - 2.f * randf();
      float sn    = std::sqrt(1-dy*dy);
      float theta = float(2.0*M_PI)*randf();
      float dx    = sn * std::cos(theta);
      float dz    = sn * std::sin(theta);

      p.dir       = Vec3(dx,dy,dz);
      break;
      }
    case ParticleFx::Dir::Dir: {
      float dirAngleHeadVar = decl.dirAngleHeadVar;
      float dirAngleElevVar = decl.dirAngleElevVar;

      // HACK: STARGATE_PARTICLES
      if(decl.dirAngleHeadVar>=180)
        dirAngleHeadVar = 0;
      if(decl.dirAngleElevVar>=180 )
        dirAngleElevVar = 0;

      float head = (90+randf(decl.dirAngleHead,dirAngleHeadVar))*float(M_PI)/180.f;
      float elev = (   randf(decl.dirAngleElev,dirAngleElevVar))*float(M_PI)/180.f;

      float dx = std::cos(elev) * std::cos(head);
      float dy = std::sin(elev);
      float dz = std::cos(elev) * std::sin(head);

      switch(decl.dirFOR) {
        case ParticleFx::Frame::Object:
        case ParticleFx::Frame::Node: {
          p.dir = emitter.direction[0]*dx +
                  emitter.direction[1]*dy +
                  emitter.direction[2]*dz;
          break;
          }
        case ParticleFx::Frame::World: {
          p.dir = Vec3(dx,dy,dz);
          break;
          }
        }
      break;
      }
    case ParticleFx::Dir::Target:
      Vec3 targetPos = emitter.pos;
      switch(decl.dirModeTargetFOR) {
        case ParticleFx::Frame::World:
          targetPos = decl.dirModeTargetPos;
          break;
        case ParticleFx::Frame::Node:
        case ParticleFx::Frame::Object: {
          if(emitter.targetNpc!=nullptr) {
            // MFX_WINDFIST_CAST
            auto mt = emitter.targetNpc->transform();
            mt.project(targetPos);
            } else {
            // IRRLICHT_DIE
            targetPos = emitter.pos;
            }
          break;
          }
        }
      p.dir += targetPos - (emitter.pos+p.pos);
      break;
    }

  if(!decl.useEmittersFOR)
    p.pos += emitter.pos;

  auto l = p.dir.length();
  if(l!=0.f) {
    float velocity = randf(decl.velAvg,decl.velVar);
    p.dir = p.dir*velocity/l;
    }
  }

void PfxBucket::finalize(size_t particle) {
  particles[particle] = {};
  pfxCpu   [particle] = {};
  }

void PfxBucket::tick(Block& sys, ImplEmitter& emitter, size_t particle, uint64_t dt) {
  ParState& ps = particles[particle+sys.offset];
  if(ps.life==0)
    return;

  if(decl.flipSteady)
    return;

  if(ps.life<=dt){
    ps.life = 0;
    sys.count--;
    finalize(particle+sys.offset);
    return;
    }

  const float dtF = float(dt);

  // eval particle
  ps.life  = uint16_t(ps.life-dt);
  if(decl.motion.active()) {
    // own curl and drag (particles.d): substeps, drag integrated exactly, the field in world space
    const Vec3  origin = decl.useEmittersFOR ? emitter.pos : Vec3();
    const float t0     = float((parent.now>=dt ? parent.now-dt : 0)%3600000u);
    PfxMotion::step(decl.motion, decl.flyGravity, ps.pos, ps.dir, origin, t0, dtF);
    } else {
    ps.pos  += ps.dir*dtF;
    ps.dir  += decl.flyGravity*dtF;
    }

  if(maxTrlTime!=0)
    tickTrail(ps,emitter,dt);
  }

void PfxBucket::tickTrail(ParState& ps, ImplEmitter& emitter, uint64_t dt) {
  for(auto& i:ps.trail)
    i.time+=dt;

  Trail tx;
  if(decl.useEmittersFOR)
    tx.pos = ps.pos + emitter.pos; else
    tx.pos = ps.pos;

  if(ps.trail.size()==0) {
    ps.trail.push_back(tx);
    }
  else if(ps.trail.back().pos!=tx.pos) {
    bool extrude = false;
    if(false && ps.trail.size()>1) {
      auto u = tx.pos              - ps.trail[ps.trail.size()-2].pos;
      auto v = ps.trail.back().pos - ps.trail[ps.trail.size()-2].pos;
      if(std::abs(Vec3::dotProduct(u,v)-u.length()*v.length()) < 0.001f)
        extrude = true;
      }
    if(extrude)
      ps.trail.back() = tx; else
      ps.trail.push_back(tx);
    }
  else {
    ps.trail.back().time = 0;
    }

  for(size_t rm=0; rm<=ps.trail.size(); ++rm) {
    if(rm==ps.trail.size() || ps.trail[rm].time<maxTrlTime) {
      ps.trail.erase(ps.trail.begin(),ps.trail.begin()+int(rm));
      break;
      }
    }
  }

void PfxBucket::tick(uint64_t dt, const Vec3& viewPos) {
  if(decl.isDecal()) {
    implTickDecals(dt,viewPos);
    } else {
    implTickCommon(dt,viewPos);
    }
  }

void PfxBucket::implTickCommon(uint64_t dt, const Vec3& viewPos) {
  bool doShrink = false;
  for(auto& emitter:impl) {
    if(emitter.st==S_Free)
      continue;

    const auto dp     = emitter.pos-viewPos;
    const bool nearby = (dp.quadLength()<PfxObjects::viewRage*PfxObjects::viewRage);

    if(emitter.next==nullptr && decl.ppsCreateEm!=nullptr && emitter.waitforNext<dt && emitter.st==S_Active) {
      emitter.next.reset(new PfxEmitter(parent,decl.ppsCreateEm));
      auto& e = *emitter.next;
      e.setPosition(emitter.pos.x,emitter.pos.y,emitter.pos.z);
      e.setActive(true);
      e.setLooped(emitter.isLoop);
      }

    if(emitter.waitforNext>=dt)
      emitter.waitforNext-=dt;

    if(emitter.flicker.active() && emitter.block!=size_t(-1)) {
      auto& p = getBlock(emitter);
      p.flicker = PfxMotion::flicker(emitter.flicker, parent.now);
      if(PfxMotion::logEnabled())
        Log::i("flicker pfx seed=", emitter.flicker.seed, " t=", parent.now, " s=", p.flicker);
      }

    if(decl.flipSteady && emitter.block!=size_t(-1) && (emitter.st!=S_Active || !nearby)) {
      // steady particles never die on their own: they go with their emitter or when it is out of view range
      auto& p = getBlock(emitter);
      for(size_t i=0;i<blockSize;++i)
        if(particles[p.offset+i].life!=0)
          finalize(p.offset+i);
      p.count = 0;
      if(emitter.st==S_Fade || !nearby) {
        freeBlock(emitter.block);
        if(emitter.st==S_Fade) {
          emitter.st = S_Free;
          updateTextureDemand(size_t(&emitter-impl.data()));
          }
        doShrink = true;
        continue;
        }
      }

    if(emitter.block!=size_t(-1)) {
      auto& p = getBlock(emitter);
      if(p.count>0) {
        for(size_t i=0;i<blockSize;++i)
          tick(p,emitter,i,dt);
        if(p.count==0 && (emitter.st==S_Fade || !nearby)) {
          // free mem
          freeBlock(emitter.block);
          if(emitter.st==S_Fade) {
            emitter.st = S_Free;
            updateTextureDemand(size_t(&emitter-impl.data()));
            }
          doShrink = true;
          continue;
          }
        }
      }

    if(emitter.st==S_Active && nearby) {
      auto& p = getBlock(emitter);
      auto dE = decl.flipSteady ? (p.count==0 ? uint64_t(blockSize) : 0) : ppsDiff(decl,emitter.isLoop,p.timeTotal,p.timeTotal+dt);
      tickEmit(p,emitter,dE);
      }

    if(emitter.block!=size_t(-1)) {
      auto& p = getBlock(emitter);
      p.timeTotal+=dt;
      }
    }

  if(doShrink)
    shrink();
  }

void PfxBucket::implTickDecals(uint64_t, const Vec3&) {
  for(auto& emitter:impl) {
    if(emitter.st==S_Free)
      continue;

    auto& p = getBlock(emitter);
    if(emitter.st==S_Active || emitter.st==S_Inactive) {
      if(p.count==0)
        tickEmit(p,emitter,1);
      } else
    if(emitter.st==S_Fade) {
      for(size_t i=0; i<blockSize; ++i)
        particles[p.offset+i].life = 0;
      p.count = 0;
      freeBlock(emitter.block);
      emitter.st = S_Free;
      updateTextureDemand(size_t(&emitter-impl.data()));
      }
    }
  }

void PfxBucket::tickEmit(Block& p, ImplEmitter& emitter, uint64_t emited) {
  size_t lastI = 0;
  for(size_t id=1; emited>0; ++id) {
    const size_t i  = id%blockSize;
    ParState&    ps = particles[i+p.offset];
    if(ps.life==0) { // free slot
      --emited;
      lastI = i;
      init(p,emitter,i+p.offset);
      if(ps.life==0)
        continue;
      p.count++;
      } else {
      // out of slots
      if(lastI==i)
        return;
      }
    }
  }

// RGB9E5: a non-negative HDR colour in 32 bits (unpackRgb9e5 in pfx.vert)
static uint32_t packRgb9e5(const Vec3& c) {
  const float maxV = 65408.f;
  const float r = std::clamp(c.x, 0.f, maxV), g = std::clamp(c.y, 0.f, maxV), b = std::clamp(c.z, 0.f, maxV);
  const float m = std::max({r, g, b});
  if(m<=1e-6f)
    return 0;
  int   e     = std::max(-16, int(std::floor(std::log2(m)))) + 1 + 15;
  float denom = std::exp2(float(e - 15 - 9));
  if(int(std::floor(m/denom + 0.5f))==512) {
    denom *= 2.f;
    ++e;
    }
  const auto q = [&](float v) { return uint32_t(std::min(511.f, std::floor(v/denom + 0.5f))); };
  return q(r) | (q(g) << 9) | (q(b) << 18) | (uint32_t(e) << 27);
  }

// Irradiance of point lights at a particle, as light.frag lights a surface facing them, a quarter of it (a thin
// puff scatters to all sides and lets most light through), without the shadow; held at 1/0.02 next to the
// source, where the inverse square would light a puff over a fire like a lamp.
static Vec3 pointLightAt(const std::vector<LightGroup::Near>& lights, const Vec3& at) {
  Vec3 sum;
  for(auto& l:lights) {
    const float factor = (l.pos-at).quadLength()/(l.range*l.range);
    if(factor>=1.f)
      continue;
    const float smooth = 1.f - factor*factor;
    sum += l.color * (0.25f/std::max(factor, 0.02f) * smooth*smooth);
    }
  return sum;
  }

void PfxBucket::buildSsbo() {
  buildSsboTrails();

  // smoke and steam (blended, lit in the forward pass) take the light of the fires next to them
  const bool lit = decl.visMaterial.alpha==Material::AlphaFunc::Transparent;
  std::vector<LightGroup::Near> lights;

  auto  colorS          = decl.visTexColorStart;
  auto  colorE          = decl.visTexColorEnd;
  auto  visSizeStart    = decl.visSizeStart;
  auto  visSizeEndScale = decl.visSizeEndScale;
  auto  visAlphaStart   = decl.visAlphaStart;
  auto  visAlphaEnd     = decl.visAlphaEnd;
  auto  visAlphaFunc    = decl.visMaterial.alpha;

  for(auto& p:block) {
    if(p.count==0)
      continue;
    if(lit)
      parent.world.lights().nearLights(p.pos, 300.f, lights);

    for(size_t pId=0; pId<blockSize; ++pId) {
      ParState& ps = particles[pId+p.offset];
      auto&     px = pfxCpu   [pId+p.offset];

      if(ps.life==0) {
        px.size = Vec3();
        continue;
        }

      const float a     = decl.flipSteady ? 0.f : ps.lifeTime();
      const Vec3  cl    = colorS*(1.f-a)        + colorE*a;
      //const float clA   = visAlphaStart*(1.f-a) + visAlphaEnd*a;
      //NOTE: particles even with harsh transition, appear to have some fade in/out anyway in bvanilla game
      const float clA   = decl.flipSteady ? visAlphaStart
                        : (std::min(a, 0.1f)/0.1f)*visAlphaStart*(1.f-a) + (std::min(1.f-a, 0.1f)/0.1f)*visAlphaEnd*a;

      const float scale = 1.f*(1.f-a) + a*visSizeEndScale;
      const float szX   = visSizeStart.x*scale;
      const float szY   = visSizeStart.y*scale;
      const float szZ   = 0.1f*((szX+szY)*0.5f);

      struct Color {
        uint8_t r=255;
        uint8_t g=255;
        uint8_t b=255;
        uint8_t a=255;
        } color;

      if(visAlphaFunc==Material::AlphaFunc::AdditiveLight) {
        color.r = uint8_t(cl.x*clA);
        color.g = uint8_t(cl.y*clA);
        color.b = uint8_t(cl.z*clA);
        color.a = uint8_t(255);
        } else {
        color.r = uint8_t(cl.x);
        color.g = uint8_t(cl.y);
        color.b = uint8_t(cl.z);
        color.a = uint8_t(clA*255);
        }
      if(p.flicker!=1.f) {
        // the flame's signal (vobs.d "flicker"): the brightness, up to white
        const float k = p.flicker * (visAlphaFunc==Material::AlphaFunc::AdditiveLight ? clA : 1.f);
        color.r = uint8_t(std::min(255.f, cl.x*k));
        color.g = uint8_t(std::min(255.f, cl.y*k));
        color.b = uint8_t(std::min(255.f, cl.z*k));
        }
      uint32_t colorU32;
      std::memcpy(&colorU32,&color,4);
      buildBilboard(px,p,ps, colorU32, szX,szY,szZ);
      px.colorB = (lit && !lights.empty()) ? packRgb9e5(pointLightAt(lights, px.pos)) : 0;
      if(decl.flipFrames>0) {
        // the atlas frame by the particle's age (a steady one: by its emitter's clock) from its own start frame
        // (a hash of its slot and birth time)
        const uint32_t seed   = uint32_t(pId+p.offset)*2654435761u ^ uint32_t(ps.maxLife)*40503u;
        const float    frames = float(decl.flipFrames);
        const float    age    = decl.flipSteady ? float(p.timeTotal%3600000u) : float(ps.maxLife-ps.life);
        const float    t      = age*decl.flipFps/1000.f + float(seed%997u)/997.f*frames;
        const float    f      = std::fmod(t, frames);
        const uint32_t f0     = uint32_t(f)%decl.flipFrames;
        const uint32_t f1     = (f0+1)%decl.flipFrames;
        const uint32_t blend  = uint32_t((f-std::floor(f))*127.f+0.5f);
        px.bits0 |= 1u << 6;
        px.bits0 |= uint32_t(decl.flipCols-1) << 7 | uint32_t(decl.flipRows-1) << 10;
        px.bits0 |= f0 << 13 | f1 << 19 | blend << 25;
        }
      }
    }
  }

void PfxBucket::buildSsboTrails() {
  if(!decl.hasTrails())
    return;

  trlCpu.reserve(trlCpu.size());
  trlCpu.clear();

  for(auto& p:particles) {
    if(p.life==0)
      continue;
    if(p.trail.size()<2)
      continue;

    float maxT = float(std::min(maxTrlTime,p.trail[0].time));
    for(size_t r=1; r<p.trail.size(); ++r) {
      PfxState st;
      buildTrailSegment(st,p.trail[r-1],p.trail[r],maxT);
      trlCpu.push_back(st);
      }
    }
  }

void PfxBucket::buildBilboard(PfxState& v, const Block& p, const ParState& ps, const uint32_t color,
                              float szX, float szY, float szZ) {
  if(decl.useEmittersFOR)
    v.pos = ps.pos + p.pos; else
    v.pos = ps.pos;

  v.size  = Vec3(szX,szY,szZ);
  v.color = color;
  v.bits0 = 0;
  v.bits0 |= uint32_t(decl.visZBias ? 1 : 0);
  v.bits0 |= uint32_t(decl.visTexIsQuadPoly ? 1 : 0) << 1;
  v.bits0 |= uint32_t(decl.visYawAlign ? 1 : 0) << 2;
  v.bits0 |= uint32_t(0) << 3; // TODO: trails
  v.bits0 |= uint32_t(decl.visOrientation) << 4;
  v.dir   = ps.dir;
  if(decl.stretchMs>0 && decl.visOrientation!=ParticleFx::Orientation::None) {
    // own streak (particles.d "stretch", pfx.vert): orientation 3, size.z the path of stretchMs at the velocity in cm
    v.bits0 |= uint32_t(3) << 4;
    v.size.z = ps.dir.length()*decl.stretchMs;
    }
  if(decl.minPixels>0) {
    // own least size on screen (particles.d "minPixels", pfx.vert), 1/16 px in bits 7..14: no flipbook here
    v.bits0 |= uint32_t(std::clamp(decl.minPixels*16.f+0.5f, 1.f, 255.f)) << 7;
    }
  }

void PfxBucket::buildTrailSegment(PfxState& v, const Trail& a, const Trail& b, float maxT) {
  float    tA  = 1.f - float(a.time)/maxT;
  float    tB  = 1.f - float(b.time)/maxT;

  uint32_t clA = mkTrailColor(tA);
  uint32_t clB = mkTrailColor(tB);

  v.pos    = a.pos;
  v.color  = clA;
  v.size   = Vec3(decl.trlWidth,tA,tB);
  v.bits0  = uint32_t(1) << 3;
  v.dir    = b.pos  - a.pos;
  v.colorB = clB;
  }

uint32_t PfxBucket::mkTrailColor(float clA) const {
  struct Color {
    uint8_t r=255;
    uint8_t g=255;
    uint8_t b=255;
    uint8_t a=255;
    } color;

  clA = std::max(0.f, std::min(clA, 1.f));

  if(decl.visMaterial.alpha==Material::AlphaFunc::AdditiveLight) {
    color.r = uint8_t(255*clA);
    color.g = uint8_t(255*clA);
    color.b = uint8_t(255*clA);
    color.a = uint8_t(255);
    }
  else if(decl.visMaterial.alpha==Material::AlphaFunc::Transparent) {
    color.r = 255;
    color.g = 255;
    color.b = 255;
    color.a = uint8_t(clA*255);
    }
  uint32_t cl;
  std::memcpy(&cl,&color,sizeof(cl));
  return cl;
  }

void PfxBucket::preFrameUpdate(const SceneGlobals& scene, uint8_t fId) {
  auto& device = Resources::device();

  if(item[fId].pMain!=nullptr || item[fId].pShadow!=nullptr) {
    // hidden staging under the hood
    auto& ssbo = item[fId].pfxGpu;
    if(pfxCpu.size()*sizeof(PfxBucket::PfxState)!=ssbo.byteSize()) {
      auto heap = decl.isDecal() ? BufferHeap::Device : BufferHeap::Upload;
      ssbo = device.ssbo(heap,pfxCpu);
      } else {
      if(!decl.isDecal() || forceUpdate[fId]) {
        // A decal buffer lives in device memory: a plain update there waits for the GPU to finish the
        // frame in flight (Metal). The copy is queued ahead of this frame's commands, nobody reads this
        // frame's buffer before it, so the wait buys nothing.
        ssbo.updateAsync(pfxCpu.data(), 0, pfxCpu.size()*sizeof(pfxCpu[0]));
        forceUpdate[fId] = false;
        }
      }
    }

  if(itemTrl[fId].pMain!=nullptr || itemTrl[fId].pShadow!=nullptr) {
    auto& ssbo = itemTrl[fId].pfxGpu;
    if(trlCpu.size()*sizeof(PfxBucket::PfxState)!=ssbo.byteSize()) {
      ssbo = device.ssbo(BufferHeap::Upload, trlCpu);
      } else {
      ssbo.update(trlCpu);
      }
    }
  }

void PfxBucket::drawGBuffer(Tempest::Encoder<Tempest::CommandBuffer>& cmd, const SceneGlobals& scene, uint8_t fId) {
  for(auto i:{Material::Solid, Material::AlphaTest}) {
    drawCommon(cmd, scene, item[fId],    SceneGlobals::V_Main, i, false);
    drawCommon(cmd, scene, itemTrl[fId], SceneGlobals::V_Main, i, true);
    }
  }

void PfxBucket::drawShadow(Tempest::Encoder<Tempest::CommandBuffer>& cmd, const SceneGlobals& scene, uint8_t fId, int layer) {
  auto view = SceneGlobals::VisCamera(SceneGlobals::V_Shadow0 + layer);
  for(auto i:{Material::Solid, Material::AlphaTest}) {
    drawCommon(cmd, scene, item[fId],    view, i, false);
    drawCommon(cmd, scene, itemTrl[fId], view, i, true);
    }
  }

void PfxBucket::drawMultiply(Tempest::Encoder<Tempest::CommandBuffer>& cmd, const SceneGlobals& scene, uint8_t fId) {
  for(auto i:{Material::Multiply, Material::Multiply2}) {
    drawCommon(cmd, scene, item[fId],    SceneGlobals::V_Main, i, false);
    drawCommon(cmd, scene, itemTrl[fId], SceneGlobals::V_Main, i, true);
    }
  }

void PfxBucket::drawTranslucent(Tempest::Encoder<Tempest::CommandBuffer>& cmd, const SceneGlobals& scene, uint8_t fId) {
  for(auto i:{/*Material::Water, */Material::Transparent, Material::AdditiveLight, Material::Ghost}) {
    drawCommon(cmd, scene, item[fId],    SceneGlobals::V_Main, i, false);
    drawCommon(cmd, scene, itemTrl[fId], SceneGlobals::V_Main, i, true);
    }
  }

void PfxBucket::drawCommon(Tempest::Encoder<Tempest::CommandBuffer>& cmd, const SceneGlobals& scene, const Draw& itm,
                           SceneGlobals::VisCamera view, Material::AlphaFunc func, bool trl) {
  if(itm.pfxGpu.isEmpty())
    return;

  if(decl.visMaterial.alpha!=func)
    return;

  const RenderPipeline* pso = nullptr;
  switch(view) {
    case SceneGlobals::V_Shadow0:
    case SceneGlobals::V_Shadow1:
    case SceneGlobals::V_Vsm:
      pso = itm.pShadow;
      break;
    case SceneGlobals::V_Main:
      pso = itm.pMain;
      break;
    case SceneGlobals::V_HiZ:
    case SceneGlobals::V_Count:
      break;
    }
  if(pso==nullptr)
    return;

  auto& mat = decl.visMaterial;
  if(view==SceneGlobals::V_Main || mat.isTextureInShadowPass()) {
    auto smp = SceneGlobals::isShadowView(SceneGlobals::VisCamera(view)) ? Sampler::trillinear() : Sampler::anisotrophy();
    if(trl) {
      cmd.setBinding(L_Diffuse, *decl.trlTexture);
      }
    else if(mat.hasFrameAnimation()) {
      auto frame = size_t((itm.timeShift+scene.tickCount)/mat.texAniFPSInv);
      auto t     = mat.frames[frame%mat.frames.size()];
      cmd.setBinding(L_Diffuse, *t);
      }
    else {
      cmd.setBinding(L_Diffuse, *mat.tex);
      }
    cmd.setBinding(L_Sampler, smp);
    }

  if(view==SceneGlobals::V_Main && mat.isShadowmapRequired()) {
    cmd.setBinding(L_Shadow0,  *scene.shadowMap[0],Resources::shadowSampler());
    cmd.setBinding(L_Shadow1,  *scene.shadowMap[1],Resources::shadowSampler());
    DrawCommands::bindSunVsm(cmd, scene, L_VsmPageTbl, L_VsmPageAtlas);
    }

  if(view==SceneGlobals::V_Main && mat.isSceneInfoRequired()) {
    cmd.setBinding(L_SceneClr, *scene.sceneColor, Sampler::bilinear(ClampMode::MirroredRepeat));
    cmd.setBinding(L_GDepth,   *scene.sceneDepth, Sampler::nearest (ClampMode::MirroredRepeat));
    }

  if(view==SceneGlobals::V_Main && (mat.alpha==Material::Transparent || mat.alpha==Material::AdditiveLight)) {
    // soft particles (main_pfx_f/_e): the opaque depth, to fade where a sprite meets the scene
    cmd.setBinding(L_GDepth, *scene.sceneDepth, Sampler::nearest(ClampMode::ClampToEdge));
    }

  if(view==SceneGlobals::V_Main && mat.isFogRequired()) {
    cmd.setBinding(L_FogLut,   *scene.fogLut,   Sampler::bilinear(ClampMode::ClampToEdge));
    cmd.setBinding(L_FogLutMs, *scene.fogLutMs, Sampler::bilinear(ClampMode::ClampToEdge));
    }

  cmd.setBinding(L_Scene, scene.uboGlobal[view]);
  cmd.setBinding(L_Pfx,   itm.pfxGpu);
  cmd.setPipeline(*pso);
  cmd.draw(nullptr, 0, 6, 0, itm.pfxGpu.byteSize()/sizeof(PfxState));
  }

#include "particlesdefinitions.h"
#include "basescripts.h"
#include "basestore.h"
#include "contentcensus.h"

#include <Tempest/Log>
#include <cmath>

#include "graphics/pfx/particlefx.h"
#include "programdata.h"
#include "resources.h"

using namespace Tempest;

ParticlesDefinitions::ParticlesDefinitions() {
  const auto& index=baseProgramIndex();
  catalog=ParticleContent::openStockCatalog(index);
  const auto base=index.dir.parent_path();
  const auto section=EffectDefinitionData::particleSection(base,EffectDefinitionData::stockParticleProgram);
  if(!catalog)
    throw BaseStore::Error(section.string()+": particles require the supported prepared catalog");

  if(auto* census=Resources::contentCensus())
    census->replaced("ParticleFx.dat","base/"+section.lexically_relative(base).generic_string());
  content=ParticleContent::load(*catalog,Resources::contentDir());
  for(const auto& error:content.errors) Log::e("particles: skipped ",error);
  }

ParticlesDefinitions::~ParticlesDefinitions() {
  }

const ParticleFx* ParticlesDefinitions::get(std::string_view name, bool relaxed) {
  if(name.empty())
    return nullptr;

  const auto cname=ParticleContent::canonicalName(name);

  std::lock_guard<std::recursive_mutex> guard(sync);
  return implGet(cname,relaxed);
  }

const ParticleFx* ParticlesDefinitions::get(const ParticleFx* base, const VisualFx::Key* key) {
  if(base==nullptr || key==nullptr)
    return base;
  std::lock_guard<std::recursive_mutex> guard(sync);
  return implGet(*base,*key);
  }

const ParticleFx* ParticlesDefinitions::implGet(std::string_view name, bool relaxed) {
  auto cname = std::string(name);
  auto it    = pfx.find(cname);
  if(it!=pfx.end())
    return it->second.get();
  auto decl=implGetDirect(name, relaxed);
  if(!decl)
    return nullptr;
  std::unique_ptr<ParticleFx> p{new ParticleFx(*decl,name)};
  if(const auto book=content.flipbooks.find(cname); book!=content.flipbooks.end()) {
    p->flipCols   = uint8_t(book->second.cols);
    p->flipRows   = uint8_t(book->second.rows);
    p->flipFrames = uint8_t(book->second.frames);
    p->flipFps    = book->second.fps;
    if(book->second.yaw)
      p->visYawAlign = true;
    p->flipSteady = book->second.steady;
    p->flipOnce   = book->second.once;
    }
  if(const auto w=content.with.find(cname); w!=content.with.end()) {
    for(const auto& other:w->second)
      if(const auto* fx=implGet(other,relaxed))
        p->with.push_back(fx);
    }
  if(const auto l=content.land.find(cname); l!=content.land.end())
    p->land = implGet(l->second,relaxed);
  if(const auto n=content.nodes.find(cname); n!=content.nodes.end())
    p->node = n->second;
  if(const auto m=content.motions.find(cname); m!=content.motions.end()) {
    // particles.d units to the engine's cm and ms
    const auto& own = m->second;
    p->own                 = true;
    p->motion.curlStrength = own.curlStrength*1e-4f;
    p->motion.curlWave     = own.curlFrequency*6.2831853f/100.f;
    p->motion.curlDrift    = own.curlSpeed*0.1f;
    p->motion.drag         = own.drag*1e-3f;
    p->stretchMs           = own.stretch*1000.f;
    p->minPixels           = own.minPixels;
    p->leanRise            = own.leanRise*0.1f;               // m/s -> cm/ms
    p->leanMaxTan          = std::tan(own.leanMaxDeg*3.14159265f/180.f);
    p->leanResponseMs      = own.leanResponse*1000.f;
    p->leanAlong           = own.leanAlong;
    p->leanGrowMs          = own.leanGrow*1000.f;
    p->leanGrowFrom        = own.leanGrowFrom;
    p->inherit             = own.inherit;
    p->wind                = own.wind;
    p->plumeCm             = own.plumeHeight;
    p->plumeRise           = own.plumeRise*0.1f;              // m/s -> cm/ms
    p->plumeWind           = own.wind>0 ? own.plumeWind/own.wind : 0.f;
    p->lightRange          = own.lightRange;
    p->lightColor          = Tempest::Vec3(own.lightColor[0],own.lightColor[1],own.lightColor[2]);
    p->lightKeys           = own.lightKeys;
    for(const auto& c:own.lightColors)
      p->lightColors.emplace_back(c[0],c[1],c[2]);
    p->lightFps            = own.lightFps;
    p->lightAt             = Tempest::Vec3(own.lightAt[0],own.lightAt[1],own.lightAt[2]);
    p->lightLoop           = own.lightLoop;
    p->hdrFrom             = own.hdrFrom;
    p->hdrTo               = own.hdrTo;
    p->hdrEdge             = own.hdrEdge;
    if(own.hazeShift>0) {
      // drawn after everything translucent, over a copy of the frame (Renderer, PfxBucket::drawHaze)
      p->hazeShift          = own.hazeShift;
      p->hazeCell           = own.hazeCell;
      p->visMaterial.alpha  = Material::Ghost;
      }
    }
  auto elt = pfx.insert(std::make_pair(std::move(cname),std::move(p)));

  return elt.first->second.get();
  }

const ParticleFx* ParticlesDefinitions::implGet(const ParticleFx& base, const VisualFx::Key& key) {
  auto it = pfxKey.find(&key);
  if(it!=pfxKey.end())
    return it->second.get();

  std::unique_ptr<ParticleFx> p{new ParticleFx(base,key)};
  auto elt = pfxKey.insert(std::make_pair(&key,std::move(p)));

  return elt.first->second.get();
  }

std::shared_ptr<Def::ParticleEffect> ParticlesDefinitions::implGetDirect(std::string_view name, bool relaxed) {
  if(name.empty())
    return nullptr;

  if(const auto effect=content.effects.find(std::string(name)); effect!=content.effects.end())
    return effect->second;

  if(name=="PFX_GAI_PEE")
    return peeStream();

  auto ret=catalog->materialize(name);
  if(ret==nullptr) {
    if(!relaxed)
      Log::e("invalid particle system: \"",name,"\"");
    return nullptr;
    }

  return ret;
  }

// The game's own stream (PEE, the S_PEE animation starts it) aimed at a kneeling hero a metre ahead
// (mercy.py's humiliation): its look and gravity, a faster throw up in the npc's frame, gone at the hero
std::shared_ptr<Def::ParticleEffect> ParticlesDefinitions::peeStream() {
  auto ret=catalog->materialize("PEE");
  if(ret==nullptr)
    return nullptr;

  // one key a hundred seconds long: the model keeps an effect only for its preferred time (MdlVisual::tick);
  // Npc::tickPeeStream stops it when the pose ends
  ret->pps_value          = PeeStream::pps;
  ret->pps_is_looping     = 1;
  ret->pps_scale_keys_s   = "1";
  ret->pps_fps            = 0.01f;
  ret->dir_mode_s         = "DIR";
  ret->dir_for_s          = "OBJECT";
  ret->dir_angle_head     = PeeStream::head;
  ret->dir_angle_head_var = PeeStream::spread;
  ret->dir_angle_elev     = PeeStream::elev;
  ret->dir_angle_elev_var = PeeStream::spread;
  ret->vel_avg            = PeeStream::vel;
  ret->vel_var            = PeeStream::vel*PeeStream::velSpread;  // a frame's particles string out
  ret->lsp_part_avg       = PeeStream::lifeMs;
  ret->lsp_part_var       = PeeStream::lifeMs*0.1f;
  ret->vis_alpha_start    = PeeStream::alphaStart;
  ret->vis_alpha_end      = PeeStream::alphaEnd;
  ret->vis_size_start_s   = PeeStream::size;
  ret->vis_name_s         = PeeStream::texture;
  ret->vis_tex_color_start_s = PeeStream::color;
  ret->vis_tex_color_end_s   = PeeStream::color;
  ret->vis_size_end_scale = PeeStream::sizeEnd;
  ret->use_emitters_for   = 0;
  return ret;
  }

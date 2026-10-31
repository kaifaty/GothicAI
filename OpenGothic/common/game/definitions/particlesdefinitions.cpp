#include "particlesdefinitions.h"

#include <Tempest/Log>

#include "graphics/pfx/particlefx.h"
#include "utils/fileext.h"
#include "gothic.h"

using namespace Tempest;

ParticlesDefinitions::ParticlesDefinitions() {
  vm = Gothic::inst().createPhoenixVm("ParticleFx.dat");
  }

ParticlesDefinitions::~ParticlesDefinitions() {
  }

const ParticleFx* ParticlesDefinitions::get(std::string_view name, bool relaxed) {
  if(name.empty())
    return nullptr;

  while(FileExt::hasExt(name,"PFX"))
    name = name.substr(0,name.size()-4);

  std::lock_guard<std::recursive_mutex> guard(sync);
  return implGet(name,relaxed);
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
  if(!vm || name.empty())
    return nullptr;

  if(name=="PFX_GAI_PEE")
    return peeStream();

  auto id = vm->find_symbol_by_name(name);
  if(id==nullptr) {
    if(!relaxed)
      Log::e("invalid particle system: \"",name,"\"");
    return nullptr;
    }

  auto ret = std::make_shared<Def::ParticleEffect>();
  ret->vis_tex_is_quadpoly = 1; // seem to be default
  vm->init_instance(ret, id);
  return ret;
  }

// The game's own stream (PEE, the S_PEE animation starts it) aimed at a kneeling hero a metre ahead
// (mercy.py's humiliation): its look and gravity, a faster throw up in the npc's frame, gone at the hero
std::shared_ptr<Def::ParticleEffect> ParticlesDefinitions::peeStream() {
  auto id = vm->find_symbol_by_name("PEE");
  if(id==nullptr)
    return nullptr;
  auto ret = std::make_shared<Def::ParticleEffect>();
  ret->vis_tex_is_quadpoly = 1;
  vm->init_instance(ret, id);
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

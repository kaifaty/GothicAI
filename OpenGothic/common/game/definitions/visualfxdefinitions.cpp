#include "visualfxdefinitions.h"
#include "definitiontypes.h"

#include <Tempest/Log>

#include "graphics/visualfx.h"
#include "gothic.h"
#include "utils/string_frm.h"

using namespace Tempest;

VisualFxDefinitions::VisualFxDefinitions() {
  vm = Gothic::inst().createPhoenixVm("VisualFx.dat");
  }

VisualFxDefinitions::~VisualFxDefinitions() {
  }

const VisualFx* VisualFxDefinitions::get(std::string_view name) {
  std::string cname = std::string(name);
  auto it = vfx.find(cname);
  if(it!=vfx.end())
    return it->second.get();

  auto def = implGet(cname);
  if(def == nullptr)
    return nullptr;

  auto ret = vfx.insert(std::make_pair<std::string,std::unique_ptr<VisualFx>>(std::move(cname),nullptr));
  std::array<std::optional<Def::ParticleEffectEmitKey>,int(SpellFxKey::Count)> keyDefs;
  std::vector<Def::ParticleEffectEmitKey> investDefs;
  static std::string_view keyName[int(SpellFxKey::Count)] = {
    "OPEN",
    "INIT",
    "CAST",
    "INVEST",
    "COLLIDE"
    };

  for(int i=0; i<int(SpellFxKey::Count); ++i) {
    string_frm kname(name,"_KEY_",keyName[i]);
    auto id = vm->find_symbol_by_name(kname);
    if(id==nullptr)
      continue;
    auto key = vm->init_instance<VmDef::ParticleEffectEmitKey>(id);
    keyDefs[size_t(i)] = key->copy();
    }

  for(int i=1; ; ++i) {
    string_frm kname(name,"_KEY_INVEST_",i);
    auto id = vm->find_symbol_by_name(kname);
    if(id==nullptr)
      break;
    auto key = vm->init_instance<VmDef::ParticleEffectEmitKey>(id);
    investDefs.emplace_back(key->copy());
    }

  ret.first->second.reset(new VisualFx(*def,keyDefs,investDefs));

  auto& vfx = *ret.first->second;
  vfx.dbgName = name.data();

  return &vfx;
  }

std::shared_ptr<Def::EffectBase> VisualFxDefinitions::implGet(std::string_view name) {
  if(!vm || name.empty())
    return nullptr;

  auto id = vm->find_symbol_by_name(name);
  if(id==nullptr) {
    Log::e("invalid visual effect: \"",name,"\"");
    return nullptr;
    }

  try {
    return std::make_shared<Def::EffectBase>(vm->init_instance<VmDef::EffectBase>(id)->copy());
    }
  catch (const daedalus::DaedalusScriptError&) {
    return nullptr;
    }
  }

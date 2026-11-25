#include "visualfxdefinitions.h"

#include <Tempest/Log>
#include <algorithm>
#include <cctype>

#include "basescripts.h"
#include "basestore.h"
#include "contentcensus.h"
#include "effectdefinitiondata.h"
#include "graphics/visualfx.h"
#include "programdata.h"
#include "resources.h"
#include "utils/string_frm.h"

using namespace Tempest;

namespace {
std::string symbolName(std::string_view name) {
  std::string upper(name);
  std::transform(upper.begin(),upper.end(),upper.begin(),[](unsigned char c) {
    return char(std::toupper(c));
    });
  return upper;
  }
}

VisualFxDefinitions::VisualFxDefinitions() {
  const auto& index = baseProgramIndex();
  const auto* source = ProgramData::visualFxProgram(index);
  if(source==nullptr || source->recordSha256!=EffectDefinitionData::stockVisualProgram)
    throw BaseStore::Error("visual effects require the supported prepared catalog");

  const auto base = Resources::baseDir();
  const auto section = EffectDefinitionData::visualSection(base,source->recordSha256);
  try {
    auto records = EffectDefinitionData::readVisuals(base,source->recordSha256);
    effects.reserve(records.effects.size());
    keys.reserve(records.keys.size());
    for(auto& effect : records.effects)
      effects.emplace(std::move(effect.name),std::move(effect.value));
    for(auto& key : records.keys)
      keys.emplace(std::move(key.name),std::move(key.value));
    if(auto* census = Resources::contentCensus())
      census->replaced("VisualFx.dat","base/"+section.lexically_relative(base).generic_string());
    }
  catch(const std::runtime_error& error) {
    throw BaseStore::Error(section.string()+": "+error.what());
    }
  }

VisualFxDefinitions::~VisualFxDefinitions() = default;

const VisualFx* VisualFxDefinitions::get(std::string_view name) {
  std::string cname = std::string(name);
  auto it = vfx.find(cname);
  if(it!=vfx.end())
    return it->second.get();

  auto def = findEffect(cname);
  if(!def)
    return nullptr;

  auto& [cachedName,cachedEffect] = *vfx.try_emplace(std::move(cname),nullptr).first;
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
    keyDefs[size_t(i)] = findKey(kname);
    }

  for(int i=1; ; ++i) {
    string_frm kname(name,"_KEY_INVEST_",i);
    auto key = findKey(kname);
    if(!key)
      break;
    investDefs.emplace_back(std::move(*key));
    }

  cachedEffect = std::make_unique<VisualFx>(*def,keyDefs,investDefs);
  cachedEffect->dbgName = cachedName.c_str();
  return cachedEffect.get();
  }

std::optional<Def::EffectBase> VisualFxDefinitions::findEffect(std::string_view name) {
  if(name.empty())
    return std::nullopt;

  const auto it = effects.find(symbolName(name));
  if(it!=effects.end())
    return it->second;
  Log::e("invalid visual effect: \"",name,"\"");
  return std::nullopt;
  }

std::optional<Def::ParticleEffectEmitKey> VisualFxDefinitions::findKey(std::string_view name) {
  const auto it = keys.find(symbolName(name));
  if(it!=keys.end())
    return it->second;
  return std::nullopt;
  }

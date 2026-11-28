#include "contentregistry.h"

#include "game/rpgvocabulary.h"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <algorithm>
#include <cctype>
#include <queue>
#include <set>

namespace modkit {

namespace {

bool isLowerIdentChar(char c) noexcept {
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
  }

bool isLocalNameChar(char c) noexcept {
  return isLowerIdentChar(c) || c == '-' || c == '.' || c == '/';
  }

int32_t categoryMainFlag(std::string_view cat) noexcept {
  if(cat == "weapon_1h" || cat == "weapon_2h" || cat == "melee")
    return int32_t(ITM_CAT_NF);
  if(cat == "bow" || cat == "crossbow" || cat == "ranged")
    return int32_t(ITM_CAT_FF);
  if(cat == "armor")
    return int32_t(ITM_CAT_ARMOR);
  if(cat == "food")
    return int32_t(ITM_CAT_FOOD);
  if(cat == "potion")
    return int32_t(ITM_CAT_POTION);
  if(cat == "magic" || cat == "ring" || cat == "amulet" || cat == "belt")
    return int32_t(ITM_CAT_MAGIC);
  if(cat == "rune")
    return int32_t(ITM_CAT_RUNE);
  if(cat == "doc" || cat == "document")
    return int32_t(ITM_CAT_DOCS);
  return int32_t(ITM_CAT_NONE);
  }

ItemFlag categoryItemFlags(std::string_view cat) noexcept {
  if(cat == "weapon_1h") return ItemFlag::SWORD;
  if(cat == "weapon_2h") return ItemFlag::TWO_HANDED_SWORD;
  if(cat == "bow")       return ItemFlag::BOW;
  if(cat == "crossbow")  return ItemFlag::CROSSBOW;
  if(cat == "ring")      return ItemFlag::RING;
  if(cat == "amulet")    return ItemFlag::AMULET;
  if(cat == "belt")      return ItemFlag::BELT;
  return ItemFlag(0);
  }

bool parsePropsObject(const rapidjson::Value& v, ItemInstanceProps& out, std::string& error) {
  if(!v.IsObject()) {
    error = "default_props must be an object";
    return false;
    }
  if(auto it = v.FindMember("quality"); it != v.MemberEnd()) {
    if(!it->value.IsNumber()) { error = "default_props.quality must be a number"; return false; }
    out.quality = float(it->value.GetDouble());
    }
  if(auto it = v.FindMember("durability"); it != v.MemberEnd()) {
    if(!it->value.IsNumber()) { error = "default_props.durability must be a number"; return false; }
    out.durability = float(it->value.GetDouble());
    }
  if(auto it = v.FindMember("max_durability"); it != v.MemberEnd()) {
    if(!it->value.IsNumber()) { error = "default_props.max_durability must be a number"; return false; }
    out.maxDurability = float(it->value.GetDouble());
    }
  if(auto it = v.FindMember("temperature"); it != v.MemberEnd()) {
    if(!it->value.IsNumber()) { error = "default_props.temperature must be a number"; return false; }
    out.temperature = float(it->value.GetDouble());
    }
  if(auto it = v.FindMember("unique"); it != v.MemberEnd()) {
    if(!it->value.IsBool()) { error = "default_props.unique must be a boolean"; return false; }
    out.unique = it->value.GetBool();
    }
  if(auto it = v.FindMember("custom"); it != v.MemberEnd()) {
    if(!it->value.IsObject()) { error = "default_props.custom must be an object"; return false; }
    for(auto m = it->value.MemberBegin(); m != it->value.MemberEnd(); ++m) {
      std::string key(m->name.GetString(), m->name.GetStringLength());
      if(m->value.IsBool())
        out.custom.emplace(std::move(key), m->value.GetBool());
      else if(m->value.IsInt64())
        out.custom.emplace(std::move(key), m->value.GetInt64());
      else if(m->value.IsNumber())
        out.custom.emplace(std::move(key), m->value.GetDouble());
      else if(m->value.IsString())
        out.custom.emplace(std::move(key), std::string(m->value.GetString(), m->value.GetStringLength()));
      else {
        error = "default_props.custom." + key + " must be scalar (int, float, bool, string)";
        return false;
        }
      }
    }
  return true;
  }

void readStringArray(const rapidjson::Value& obj, const char* field, std::vector<std::string>& out) {
  auto it = obj.FindMember(field);
  if(it == obj.MemberEnd() || !it->value.IsArray())
    return;
  for(const auto& el : it->value.GetArray()) {
    if(el.IsString())
      out.emplace_back(el.GetString(), el.GetStringLength());
    }
  }

} // namespace

bool ContentRegistry::isValidNamespace(std::string_view ns) noexcept {
  if(ns.empty() || ns.size() > 64)
    return false;
  if(!(ns.front() >= 'a' && ns.front() <= 'z'))
    return false;
  for(char c : ns) {
    if(!isLowerIdentChar(c))
      return false;
    }
  return true;
  }

bool ContentRegistry::isValidCanonicalId(std::string_view id) noexcept {
  const size_t colon = id.find(':');
  if(colon == std::string_view::npos || colon == 0 || colon + 1 >= id.size())
    return false;
  if(id.find(':', colon + 1) != std::string_view::npos)
    return false;
  const std::string_view ns   = id.substr(0, colon);
  const std::string_view name = id.substr(colon + 1);
  if(!isValidNamespace(ns) || name.size() > 128)
    return false;
  for(char c : name) {
    if(!isLocalNameChar(c))
      return false;
    }
  return true;
  }

std::string_view ContentRegistry::namespaceOf(std::string_view id) noexcept {
  const size_t colon = id.find(':');
  return colon == std::string_view::npos ? std::string_view{} : id.substr(0, colon);
  }

bool ContentRegistry::parseManifestJson(std::string_view jsonText, ModManifest& out, std::string& error) {
  rapidjson::Document doc;
  doc.Parse(jsonText.data(), jsonText.size());
  if(doc.HasParseError() || !doc.IsObject()) {
    error = "Invalid mod.json: expected JSON object";
    return false;
    }
  auto idIt = doc.FindMember("id");
  if(idIt == doc.MemberEnd() || !idIt->value.IsString()) {
    error = "mod.json missing string field 'id'";
    return false;
    }
  out = ModManifest{};
  out.id.assign(idIt->value.GetString(), idIt->value.GetStringLength());
  if(!isValidNamespace(out.id)) {
    error = "Invalid mod id '" + out.id + "': must match [a-z][a-z0-9_]*";
    return false;
    }
  if(auto it = doc.FindMember("version"); it != doc.MemberEnd() && it->value.IsString())
    out.version.assign(it->value.GetString(), it->value.GetStringLength());
  else {
    error = "mod.json missing string field 'version'";
    return false;
    }
  if(auto it = doc.FindMember("sdk_api"); it != doc.MemberEnd() && it->value.IsString())
    out.sdkApi.assign(it->value.GetString(), it->value.GetStringLength());
  if(auto it = doc.FindMember("state_schema"); it != doc.MemberEnd() && it->value.IsUint())
    out.stateSchema = it->value.GetUint();

  if(auto it = doc.FindMember("dependencies"); it != doc.MemberEnd()) {
    if(it->value.IsObject()) {
      for(auto m = it->value.MemberBegin(); m != it->value.MemberEnd(); ++m)
        out.dependencies.emplace_back(m->name.GetString(), m->name.GetStringLength());
      } else if(it->value.IsArray()) {
      readStringArray(doc, "dependencies", out.dependencies);
      }
    }
  readStringArray(doc, "load_after", out.loadAfter);
  readStringArray(doc, "conflicts", out.conflicts);
  readStringArray(doc, "permissions", out.permissions);
  readStringArray(doc, "data", out.dataFiles);
  readStringArray(doc, "patches", out.patchFiles);
  return true;
  }

bool ContentRegistry::resolveLoadOrder(const std::vector<ModManifest>& mods,
                                       std::vector<ModManifest>& outOrdered,
                                       std::string& error) {
  outOrdered.clear();
  std::map<std::string, size_t> byId;
  for(size_t i = 0; i < mods.size(); ++i) {
    if(!isValidNamespace(mods[i].id)) {
      error = "Invalid mod id: " + mods[i].id;
      return false;
      }
    if(!byId.emplace(mods[i].id, i).second) {
      error = "Duplicate mod id: " + mods[i].id;
      return false;
      }
    }

  // Check hard dependencies and conflicts.
  for(const auto& m : mods) {
    for(const auto& dep : m.dependencies) {
      if(byId.find(dep) == byId.end()) {
        error = "Mod '" + m.id + "' requires missing dependency '" + dep + "'";
        return false;
        }
      }
    for(const auto& c : m.conflicts) {
      if(byId.find(c) != byId.end()) {
        error = "Mod '" + m.id + "' conflicts with active mod '" + c + "'";
        return false;
        }
      }
    }

  // Build directed graph: edge u -> v means u must load before v.
  std::vector<std::set<size_t>> adj(mods.size());
  std::vector<size_t> indeg(mods.size(), 0);
  auto addEdge = [&](size_t u, size_t v) {
    if(u != v && adj[u].insert(v).second)
      indeg[v]++;
    };

  auto baseIt = byId.find("base_gothic");
  for(size_t i = 0; i < mods.size(); ++i) {
    if(baseIt != byId.end() && i != baseIt->second)
      addEdge(baseIt->second, i);
    for(const auto& dep : mods[i].dependencies)
      addEdge(byId.at(dep), i);
    for(const auto& after : mods[i].loadAfter) {
      if(auto it = byId.find(after); it != byId.end())
        addEdge(it->second, i);
      }
    }

  // Min-heap keyed by mod id for deterministic lexicographical tie-breaking.
  struct Ready {
    std::string id;
    size_t      idx;
    bool operator>(const Ready& o) const { return id > o.id; }
    };
  std::priority_queue<Ready, std::vector<Ready>, std::greater<Ready>> ready;
  for(size_t i = 0; i < mods.size(); ++i) {
    if(indeg[i] == 0)
      ready.push({mods[i].id, i});
    }

  while(!ready.empty()) {
    const auto cur = ready.top();
    ready.pop();
    outOrdered.push_back(mods[cur.idx]);
    for(size_t next : adj[cur.idx]) {
      if(--indeg[next] == 0)
        ready.push({mods[next].id, next});
      }
    }

  if(outOrdered.size() != mods.size()) {
    error = "Cyclic mod dependency or load_after ordering detected";
    outOrdered.clear();
    return false;
    }
  return true;
  }

bool ContentRegistry::registerStockItem(std::string canonicalId, uint32_t fixedId, ItemRecord record, std::string& error) {
  if(frozen) {
    error = "ContentRegistry is frozen";
    return false;
    }
  if(!isValidCanonicalId(canonicalId)) {
    error = "Invalid stock item canonical ID: " + canonicalId;
    return false;
    }
  if(itemsById.find(canonicalId) != itemsById.end()) {
    error = "Duplicate item ID: " + canonicalId;
    return false;
    }
  if(itemIdByRuntime.find(fixedId) != itemIdByRuntime.end()) {
    error = "Duplicate stock item runtime ID: " + std::to_string(fixedId);
    return false;
    }
  ItemArchetype def;
  def.id        = std::move(canonicalId);
  def.runtimeId = fixedId;
  def.record    = std::move(record);
  def.ownerMod  = std::string(namespaceOf(def.id));
  itemIdByRuntime.emplace(fixedId, def.id);
  itemsById.emplace(def.id, std::move(def));
  return true;
  }

bool ContentRegistry::registerItem(ItemArchetype def, std::string& error) {
  if(frozen) {
    error = "ContentRegistry is frozen";
    return false;
    }
  if(!isValidCanonicalId(def.id)) {
    error = "Invalid item canonical ID: " + def.id;
    return false;
    }
  const auto ns = namespaceOf(def.id);
  if(!def.ownerMod.empty() && ns != def.ownerMod) {
    error = "Mod '" + def.ownerMod + "' cannot declare item '" + def.id + "' in foreign namespace";
    return false;
    }
  if(itemsById.find(def.id) != itemsById.end()) {
    error = "Duplicate item ID: " + def.id;
    return false;
    }
  if(def.ownerMod.empty())
    def.ownerMod = std::string(ns);
  if(!def.stackable) {
    if(!def.defaultProps.has_value())
      def.defaultProps = ItemInstanceProps{};
    def.defaultProps->unique = true;
    }
  while(itemIdByRuntime.find(nextModItemId) != itemIdByRuntime.end())
    ++nextModItemId;
  def.runtimeId = nextModItemId++;
  itemIdByRuntime.emplace(def.runtimeId, def.id);
  itemsById.emplace(def.id, std::move(def));
  return true;
  }

bool ContentRegistry::registerRecipe(RecipeDef def, std::string& error) {
  if(frozen) {
    error = "ContentRegistry is frozen";
    return false;
    }
  if(!isValidCanonicalId(def.id)) {
    error = "Invalid recipe canonical ID: " + def.id;
    return false;
    }
  const auto ns = namespaceOf(def.id);
  if(!def.ownerMod.empty() && ns != def.ownerMod) {
    error = "Mod '" + def.ownerMod + "' cannot declare recipe '" + def.id + "' in foreign namespace";
    return false;
    }
  if(recipesById.find(def.id) != recipesById.end()) {
    error = "Duplicate recipe ID: " + def.id;
    return false;
    }
  if(def.stationScheme.empty()) {
    error = "Recipe '" + def.id + "' missing station_scheme";
    return false;
    }
  if(def.outputs.empty()) {
    error = "Recipe '" + def.id + "' must produce at least one output";
    return false;
    }
  if(def.ownerMod.empty())
    def.ownerMod = std::string(ns);
  recipesById.emplace(def.id, std::move(def));
  return true;
  }

bool ContentRegistry::loadDataJson(const std::string& modId, std::string_view jsonText, std::string& error) {
  if(frozen) {
    error = "ContentRegistry is frozen";
    return false;
    }
  rapidjson::Document doc;
  doc.Parse(jsonText.data(), jsonText.size());
  if(doc.HasParseError() || !doc.IsObject()) {
    error = "Invalid data JSON for mod '" + modId + "'";
    return false;
    }

  if(auto itemsIt = doc.FindMember("items"); itemsIt != doc.MemberEnd()) {
    if(!itemsIt->value.IsArray()) {
      error = "Field 'items' must be an array";
      return false;
      }
    for(const auto& itVal : itemsIt->value.GetArray()) {
      if(!itVal.IsObject()) {
        error = "Each item entry must be an object";
        return false;
        }
      auto idIt = itVal.FindMember("id");
      if(idIt == itVal.MemberEnd() || !idIt->value.IsString()) {
        error = "Item entry missing string 'id'";
        return false;
        }
      ItemArchetype def;
      def.id.assign(idIt->value.GetString(), idIt->value.GetStringLength());
      def.ownerMod = modId;
      if(auto it = itVal.FindMember("name"); it != itVal.MemberEnd() && it->value.IsString())
        def.record.name.assign(it->value.GetString(), it->value.GetStringLength());
      if(auto it = itVal.FindMember("description"); it != itVal.MemberEnd() && it->value.IsString())
        def.record.description.assign(it->value.GetString(), it->value.GetStringLength());
      else
        def.record.description = def.record.name;
      if(auto it = itVal.FindMember("visual"); it != itVal.MemberEnd() && it->value.IsString())
        def.record.visual.assign(it->value.GetString(), it->value.GetStringLength());
      if(auto it = itVal.FindMember("base_value"); it != itVal.MemberEnd() && it->value.IsInt())
        def.record.value = it->value.GetInt();
      if(auto it = itVal.FindMember("category"); it != itVal.MemberEnd() && it->value.IsString()) {
        const std::string_view cat(it->value.GetString(), it->value.GetStringLength());
        def.record.main_flag = categoryMainFlag(cat);
        def.record.flags     = categoryItemFlags(cat);
        }
      if(auto it = itVal.FindMember("damage"); it != itVal.MemberEnd() && it->value.IsObject()) {
        int32_t total = 0;
        int32_t mask  = 0;
        auto addDmg = [&](const char* key, uint32_t idx) {
          if(auto d = it->value.FindMember(key); d != it->value.MemberEnd() && d->value.IsInt()) {
            const int32_t val = d->value.GetInt();
            if(val > 0) {
              def.record.damage[idx] = val;
              total += val;
              mask  |= int32_t(1u << idx);
              }
            }
          };
        addDmg("blunt",  DamageType::BLUNT);
        addDmg("edge",   DamageType::EDGE);
        addDmg("fire",   DamageType::FIRE);
        addDmg("fly",    DamageType::FLY);
        addDmg("magic",  DamageType::MAGIC);
        addDmg("point",  DamageType::POINT);
        def.record.damage_total = total;
        def.record.damage_type  = mask;
        }
      if(auto it = itVal.FindMember("stackable"); it != itVal.MemberEnd() && it->value.IsBool())
        def.stackable = it->value.GetBool();
      readStringArray(itVal, "ai_tags", def.aiTags);
      if(auto it = itVal.FindMember("default_props"); it != itVal.MemberEnd()) {
        ItemInstanceProps p;
        if(!parsePropsObject(it->value, p, error))
          return false;
        def.defaultProps = std::move(p);
        }
      if(!registerItem(std::move(def), error))
        return false;
      }
    }

  if(auto recIt = doc.FindMember("recipes"); recIt != doc.MemberEnd()) {
    if(!recIt->value.IsArray()) {
      error = "Field 'recipes' must be an array";
      return false;
      }
    for(const auto& rVal : recIt->value.GetArray()) {
      if(!rVal.IsObject()) {
        error = "Each recipe entry must be an object";
        return false;
        }
      auto idIt = rVal.FindMember("id");
      if(idIt == rVal.MemberEnd() || !idIt->value.IsString()) {
        error = "Recipe entry missing string 'id'";
        return false;
        }
      RecipeDef def;
      def.id.assign(idIt->value.GetString(), idIt->value.GetStringLength());
      def.ownerMod = modId;
      if(auto it = rVal.FindMember("title"); it != rVal.MemberEnd() && it->value.IsString())
        def.title.assign(it->value.GetString(), it->value.GetStringLength());
      if(auto it = rVal.FindMember("station_scheme"); it != rVal.MemberEnd() && it->value.IsString())
        def.stationScheme.assign(it->value.GetString(), it->value.GetStringLength());
      if(auto it = rVal.FindMember("requires_tool"); it != rVal.MemberEnd() && it->value.IsString())
        def.requiresTool.assign(it->value.GetString(), it->value.GetStringLength());
      if(auto it = rVal.FindMember("ingredients"); it != rVal.MemberEnd() && it->value.IsArray()) {
        for(const auto& ing : it->value.GetArray()) {
          if(!ing.IsObject()) continue;
          RecipeIngredient ri;
          if(auto m = ing.FindMember("item"); m != ing.MemberEnd() && m->value.IsString())
            ri.itemId.assign(m->value.GetString(), m->value.GetStringLength());
          if(auto m = ing.FindMember("count"); m != ing.MemberEnd() && m->value.IsUint())
            ri.count = m->value.GetUint();
          if(auto m = ing.FindMember("min_quality"); m != ing.MemberEnd() && m->value.IsNumber())
            ri.minQuality = float(m->value.GetDouble());
          if(auto m = ing.FindMember("min_temperature"); m != ing.MemberEnd() && m->value.IsNumber())
            ri.minTemperature = float(m->value.GetDouble());
          def.ingredients.push_back(std::move(ri));
          }
        }
      if(auto it = rVal.FindMember("outputs"); it != rVal.MemberEnd() && it->value.IsArray()) {
        for(const auto& outVal : it->value.GetArray()) {
          if(!outVal.IsObject()) continue;
          RecipeOutput ro;
          if(auto m = outVal.FindMember("item"); m != outVal.MemberEnd() && m->value.IsString())
            ro.itemId.assign(m->value.GetString(), m->value.GetStringLength());
          if(auto m = outVal.FindMember("count"); m != outVal.MemberEnd() && m->value.IsUint())
            ro.count = m->value.GetUint();
          def.outputs.push_back(std::move(ro));
          }
        }
      if(auto it = rVal.FindMember("hooks"); it != rVal.MemberEnd() && it->value.IsObject()) {
        if(auto m = it->value.FindMember("can_craft"); m != it->value.MemberEnd() && m->value.IsString())
          def.canCraftHook.assign(m->value.GetString(), m->value.GetStringLength());
        if(auto m = it->value.FindMember("compute_props"); m != it->value.MemberEnd() && m->value.IsString())
          def.computePropsHook.assign(m->value.GetString(), m->value.GetStringLength());
        }
      if(!registerRecipe(std::move(def), error))
        return false;
      }
    }
  return true;
  }

bool ContentRegistry::applyPatchesJson(const std::string& modId, std::string_view jsonText,
                                       std::vector<std::string>& warnings, std::string& error) {
  if(frozen) {
    error = "ContentRegistry is frozen";
    return false;
    }
  rapidjson::Document doc;
  doc.Parse(jsonText.data(), jsonText.size());
  if(doc.HasParseError() || !doc.IsObject()) {
    error = "Invalid patch JSON for mod '" + modId + "'";
    return false;
    }
  auto pIt = doc.FindMember("patches");
  if(pIt == doc.MemberEnd() || !pIt->value.IsArray()) {
    error = "Patch file missing array 'patches'";
    return false;
    }

  auto noteField = [&](const std::string& target, const std::string& field) {
    auto key = std::make_pair(target, field);
    if(auto prev = patchProvenance.find(key); prev != patchProvenance.end() && prev->second != modId) {
      warnings.push_back("Patch conflict on " + target + "." + field + ": '" + modId +
                         "' overwrites '" + prev->second + "'");
      }
    patchProvenance[std::move(key)] = modId;
    };

  for(const auto& patch : pIt->value.GetArray()) {
    if(!patch.IsObject()) {
      error = "Patch entry must be an object";
      return false;
      }
    auto tIt  = patch.FindMember("target");
    auto opIt = patch.FindMember("op");
    if(tIt == patch.MemberEnd() || !tIt->value.IsString() ||
       opIt == patch.MemberEnd() || !opIt->value.IsString()) {
      error = "Patch entry requires string 'target' and 'op'";
      return false;
      }
    const std::string target(tIt->value.GetString(), tIt->value.GetStringLength());
    const std::string_view op(opIt->value.GetString(), opIt->value.GetStringLength());

    if(auto itemIt = itemsById.find(target); itemIt != itemsById.end()) {
      auto& arch = itemIt->second;
      if(op == "merge") {
        auto valIt = patch.FindMember("set");
        if(valIt == patch.MemberEnd() || !valIt->value.IsObject()) {
          error = "Patch 'merge' on item '" + target + "' requires object 'set'";
          return false;
          }
        for(auto m = valIt->value.MemberBegin(); m != valIt->value.MemberEnd(); ++m) {
          const std::string field(m->name.GetString(), m->name.GetStringLength());
          noteField(target, field);
          if(field == "base_value" && m->value.IsInt())
            arch.record.value = m->value.GetInt();
          else if(field == "name" && m->value.IsString())
            arch.record.name.assign(m->value.GetString(), m->value.GetStringLength());
          else if(field == "description" && m->value.IsString())
            arch.record.description.assign(m->value.GetString(), m->value.GetStringLength());
          else if(field == "stackable" && m->value.IsBool()) {
            arch.stackable = m->value.GetBool();
            if(!arch.stackable) {
              if(!arch.defaultProps) arch.defaultProps = ItemInstanceProps{};
              arch.defaultProps->unique = true;
              }
            }
          else {
            error = "Unsupported item patch field '" + field + "' on '" + target + "'";
            return false;
            }
          }
        } else {
        error = "Unsupported item patch op '" + std::string(op) + "' on '" + target + "'";
        return false;
        }
      continue;
      }

    if(auto recIt = recipesById.find(target); recIt != recipesById.end()) {
      auto& rec = recIt->second;
      if(op == "add_ingredient") {
        auto itemM  = patch.FindMember("item");
        auto countM = patch.FindMember("count");
        if(itemM == patch.MemberEnd() || !itemM->value.IsString()) {
          error = "Patch 'add_ingredient' requires string 'item'";
          return false;
          }
        RecipeIngredient ri;
        ri.itemId.assign(itemM->value.GetString(), itemM->value.GetStringLength());
        ri.count = (countM != patch.MemberEnd() && countM->value.IsUint()) ? countM->value.GetUint() : 1u;
        noteField(target, "ingredients");
        rec.ingredients.push_back(std::move(ri));
        } else if(op == "remove_ingredient") {
        auto itemM = patch.FindMember("item");
        if(itemM == patch.MemberEnd() || !itemM->value.IsString()) {
          error = "Patch 'remove_ingredient' requires string 'item'";
          return false;
          }
        const std::string_view rm(itemM->value.GetString(), itemM->value.GetStringLength());
        noteField(target, "ingredients");
        rec.ingredients.erase(
          std::remove_if(rec.ingredients.begin(), rec.ingredients.end(),
                         [&](const RecipeIngredient& x) { return x.itemId == rm; }),
          rec.ingredients.end());
        } else {
        error = "Unsupported recipe patch op '" + std::string(op) + "' on '" + target + "'";
        return false;
        }
      continue;
      }

    error = "Patch target not found: '" + target + "'";
    return false;
    }
  return true;
  }

bool ContentRegistry::freeze(std::string& error) {
  if(frozen)
    return true;
  for(const auto& [id, rec] : recipesById) {
    if(!rec.requiresTool.empty() && itemsById.find(rec.requiresTool) == itemsById.end()) {
      error = "Recipe '" + id + "' references unknown tool '" + rec.requiresTool + "'";
      return false;
      }
    for(const auto& ing : rec.ingredients) {
      if(ing.count == 0 || itemsById.find(ing.itemId) == itemsById.end()) {
        error = "Recipe '" + id + "' references invalid ingredient '" + ing.itemId + "'";
        return false;
        }
      }
    for(const auto& out : rec.outputs) {
      if(out.count == 0 || itemsById.find(out.itemId) == itemsById.end()) {
        error = "Recipe '" + id + "' references invalid output '" + out.itemId + "'";
        return false;
        }
      }
    }
  frozen = true;
  return true;
  }

void ContentRegistry::clear() {
  frozen        = false;
  nextModItemId = ModItemIdBase;
  itemsById.clear();
  itemIdByRuntime.clear();
  recipesById.clear();
  patchProvenance.clear();
  }

const ItemArchetype* ContentRegistry::findItem(std::string_view canonicalId) const {
  auto it = itemsById.find(canonicalId);
  return it != itemsById.end() ? &it->second : nullptr;
  }

const ItemArchetype* ContentRegistry::findItemByRuntimeId(uint32_t runtimeId) const {
  auto it = itemIdByRuntime.find(runtimeId);
  if(it == itemIdByRuntime.end())
    return nullptr;
  return findItem(it->second);
  }

std::optional<uint32_t> ContentRegistry::runtimeIdOfItem(std::string_view canonicalId) const {
  if(const auto* item = findItem(canonicalId))
    return item->runtimeId;
  return std::nullopt;
  }

const RecipeDef* ContentRegistry::findRecipe(std::string_view canonicalId) const {
  auto it = recipesById.find(canonicalId);
  return it != recipesById.end() ? &it->second : nullptr;
  }

std::vector<const RecipeDef*> ContentRegistry::recipesForStation(std::string_view stationScheme) const {
  auto eqIgnoreCase = [](std::string_view a, std::string_view b) noexcept {
    if(a.size() != b.size())
      return false;
    for(size_t i = 0; i < a.size(); ++i) {
      if(std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
        return false;
      }
    return true;
    };
  std::vector<const RecipeDef*> out;
  for(const auto& [id, rec] : recipesById) {
    if(eqIgnoreCase(rec.stationScheme, stationScheme))
      out.push_back(&rec);
    }
  return out;
  }

std::string ContentRegistry::serializeManifestJson(const std::vector<ModManifest>& activeMods) const {
  rapidjson::StringBuffer sb;
  rapidjson::Writer<rapidjson::StringBuffer> w(sb);
  w.StartObject();
  w.Key("format");
  w.Int(ManifestFormat);
  w.Key("mods");
  w.StartArray();
  for(const auto& m : activeMods) {
    w.StartObject();
    w.Key("id");           w.String(m.id.data(), rapidjson::SizeType(m.id.size()));
    w.Key("version");      w.String(m.version.data(), rapidjson::SizeType(m.version.size()));
    w.Key("state_schema"); w.Uint(m.stateSchema);
    w.EndObject();
    }
  w.EndArray();
  w.Key("registry_map");
  w.StartObject();
  for(const auto& [id, item] : itemsById) {
    w.Key(id.data(), rapidjson::SizeType(id.size()));
    w.Uint(item.runtimeId);
    }
  w.EndObject();
  w.EndObject();
  return std::string(sb.GetString(), sb.GetSize());
  }

bool ContentRegistry::buildSaveRemap(std::string_view savedManifestJson,
                                     std::unordered_map<uint32_t, uint32_t>& oldToNew,
                                     std::vector<std::string>& missingArchetypes,
                                     std::string& error) const {
  oldToNew.clear();
  missingArchetypes.clear();

  rapidjson::Document doc;
  doc.Parse(savedManifestJson.data(), savedManifestJson.size());
  if(doc.HasParseError() || !doc.IsObject()) {
    error = "Invalid saved mods/manifest.json";
    return false;
    }
  auto fmtIt = doc.FindMember("format");
  if(fmtIt == doc.MemberEnd() || !fmtIt->value.IsInt() || fmtIt->value.GetInt() != ManifestFormat) {
    error = "Unsupported mods/manifest.json format";
    return false;
    }
  auto mapIt = doc.FindMember("registry_map");
  if(mapIt == doc.MemberEnd() || !mapIt->value.IsObject()) {
    error = "Saved mods/manifest.json missing object 'registry_map'";
    return false;
    }
  for(auto m = mapIt->value.MemberBegin(); m != mapIt->value.MemberEnd(); ++m) {
    if(!m->value.IsUint()) {
      error = "Invalid runtime ID in saved registry_map";
      return false;
      }
    const std::string_view canonicalId(m->name.GetString(), m->name.GetStringLength());
    const uint32_t savedId = m->value.GetUint();
    if(const auto* cur = findItem(canonicalId)) {
      oldToNew[savedId] = cur->runtimeId;
      } else {
      missingArchetypes.emplace_back(canonicalId);
      }
    }
  return true;
  }

} // namespace modkit

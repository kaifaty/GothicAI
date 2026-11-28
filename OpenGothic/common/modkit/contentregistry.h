#pragma once

#include "itemprops.h"
#include "world/records/itemrecord.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace modkit {

struct ModManifest final {
  std::string              id;
  std::string              version;
  std::string              sdkApi = "^1.0";
  uint32_t                 stateSchema = 1;
  std::vector<std::string> dependencies;
  std::vector<std::string> loadAfter;
  std::vector<std::string> conflicts;
  std::vector<std::string> permissions;
  std::vector<std::string> dataFiles;
  std::vector<std::string> patchFiles;
  };

struct ItemArchetype final {
  std::string                      id;        // canonical "namespace:name"
  uint32_t                         runtimeId = 0;
  ItemRecord                       record{};
  bool                             stackable = true;
  std::optional<ItemInstanceProps> defaultProps;
  std::vector<std::string>         aiTags;
  std::string                      ownerMod;
  };

struct RecipeIngredient final {
  std::string          itemId;
  uint32_t             count = 1;
  std::optional<float> minQuality;
  std::optional<float> minTemperature;
  };

struct RecipeOutput final {
  std::string itemId;
  uint32_t    count = 1;
  };

struct RecipeDef final {
  std::string                   id;            // canonical "namespace:name"
  std::string                   title;
  std::string                   stationScheme; // e.g. "BSANVIL", "BSFIRE"
  std::string                   requiresTool;  // optional canonical item ID
  std::vector<RecipeIngredient> ingredients;
  std::vector<RecipeOutput>     outputs;
  std::string                   canCraftHook;
  std::string                   computePropsHook;
  std::string                   ownerMod;
  };

// Two-pass Data Stage content registry (RPG layer, immutable once frozen).
// Maps canonical string IDs ("namespace:name") to compact session runtime uint32_t IDs.
// Save files store `mods/manifest.json` with `registry_map` so saves survive mod load-order shifts.
class ContentRegistry final {
  public:
    static constexpr uint32_t ModItemIdBase = 0x10000000u;
    static constexpr int      ManifestFormat = 1;

    static bool isValidNamespace(std::string_view ns) noexcept;
    static bool isValidCanonicalId(std::string_view id) noexcept;
    static std::string_view namespaceOf(std::string_view id) noexcept;

    // Deterministic topological load-order resolution:
    // base_gothic first -> explicit dependencies -> loadAfter -> lexicographical by mod id.
    static bool resolveLoadOrder(const std::vector<ModManifest>& mods,
                                 std::vector<ModManifest>& outOrdered,
                                 std::string& error);

    static bool parseManifestJson(std::string_view jsonText, ModManifest& out, std::string& error);

    bool isFrozen() const noexcept { return frozen; }

    // Registers a stock Gothic II item with its fixed catalog numeric ID (namespace "gothic:<lower>").
    bool registerStockItem(std::string canonicalId, uint32_t fixedId, ItemRecord record, std::string& error);

    // Data Stage Pass 1: register new mod-owned item/recipe in the mod's own namespace.
    bool registerItem(ItemArchetype def, std::string& error);
    bool registerRecipe(RecipeDef def, std::string& error);

    // Data Stage Pass 1 JSON loader (`data/*.json`: {"items": [...], "recipes": [...]}).
    bool loadDataJson(const std::string& modId, std::string_view jsonText, std::string& error);

    // Data Stage Pass 2 JSON patches (`patches/*.json`: {"patches": [...]}).
    bool applyPatchesJson(const std::string& modId, std::string_view jsonText,
                          std::vector<std::string>& warnings, std::string& error);

    // Validates cross-references (recipe ingredients/outputs/tools exist) and locks registry immutable.
    bool freeze(std::string& error);

    void clear();

    // Runtime read-only lookups.
    const ItemArchetype* findItem(std::string_view canonicalId) const;
    const ItemArchetype* findItemByRuntimeId(uint32_t runtimeId) const;
    std::optional<uint32_t> runtimeIdOfItem(std::string_view canonicalId) const;

    const RecipeDef* findRecipe(std::string_view canonicalId) const;
    std::vector<const RecipeDef*> recipesForStation(std::string_view stationScheme) const;

    size_t itemCount()   const noexcept { return itemsById.size(); }
    size_t recipeCount() const noexcept { return recipesById.size(); }

    // Serializes active mod manifests and the canonical string->runtimeId map (`mods/manifest.json`).
    std::string serializeManifestJson(const std::vector<ModManifest>& activeMods) const;

    // Builds an old_runtime_id -> new_runtime_id remap table from a saved `mods/manifest.json`.
    // Archetypes that were removed from the active mod set are reported in `missingArchetypes`
    // rather than failing the load.
    bool buildSaveRemap(std::string_view savedManifestJson,
                        std::unordered_map<uint32_t, uint32_t>& oldToNew,
                        std::vector<std::string>& missingArchetypes,
                        std::string& error) const;

  private:
    bool frozen = false;
    uint32_t nextModItemId = ModItemIdBase;

    std::map<std::string, ItemArchetype, std::less<>> itemsById;
    std::unordered_map<uint32_t, std::string>         itemIdByRuntime;
    std::map<std::string, RecipeDef, std::less<>>     recipesById;
    // Tracks last modifier per (targetId, field) to emit deterministic multi-mod conflict warnings.
    std::map<std::pair<std::string, std::string>, std::string> patchProvenance;
  };

} // namespace modkit

#include "modkit/contentregistry.h"
#include "modkit/entityid.h"
#include "modkit/itemprops.h"
#include "commandline.h"
#include "game/gamesession.h"
#include "game/inventory.h"
#include "game/savegameheader.h"
#include "game/serialize.h"
#include "gamemusic.h"
#include "gothic.h"
#include "resources.h"
#include "savearchive.h"
#include "world/objects/interactive.h"
#include "world/objects/item.h"
#include "world/objects/npc.h"
#include "world/world.h"

#include <Tempest/MemReader>
#include <Tempest/MemWriter>
#include <Tempest/Pixmap>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

void require(bool condition, const char* message) {
  if(condition)
    return;
  std::fprintf(stderr,"FAIL: %s\n",message);
  std::exit(EXIT_FAILURE);
  }

void testIdGenerator() {
  modkit::IdGenerator gen;
  const auto e1 = gen.allocEntity();
  const auto e2 = gen.allocEntity();
  const auto i1 = gen.allocItem();
  const auto i2 = gen.allocItem();
  require(e1.valid() && e1.raw==1 && e2.raw==2,"EntityId must allocate monotonically from 1");
  require(i1.valid() && i1.raw==1 && i2.raw==2,"ItemUid must allocate monotonically from 1");
  require(e1<e2 && i1<i2,"IDs must support ordering");

  std::unordered_set<modkit::EntityId> entSet{e1,e2};
  std::unordered_set<modkit::ItemUid>  itmSet{i1,i2};
  require(entSet.size()==2 && itmSet.size()==2,"IDs must support std::hash");

  gen.observeEntity(modkit::EntityId{50});
  gen.observeEntity(modkit::EntityId{10});
  require(gen.allocEntity().raw==51,"observeEntity must advance generator monotonically");
  gen.observeItem(modkit::ItemUid{77});
  require(gen.allocItem().raw==78,"observeItem must advance generator monotonically");

  std::vector<uint8_t> bytes;
  {
    Tempest::MemWriter wr{bytes};
    SaveArchive out{wr};
    out.setEntry("game/modkit-ids");
    gen.save(out);
  }
  {
    Tempest::MemReader rd{bytes};
    SaveArchive in{rd};
    require(in.setEntry("game/modkit-ids"),"saved modkit-ids entry must exist");
    modkit::IdGenerator loaded;
    require(loaded.load(in),"IdGenerator::load must succeed on valid entry");
    require(loaded.allocEntity().raw==52,"loaded EntityId counter must continue");
    require(loaded.allocItem().raw==79,"loaded ItemUid counter must continue");
  }
  std::puts("PASS: IdGenerator");
  }

void testItemInstanceProps() {
  using modkit::ItemInstanceProps;
  ItemInstanceProps emptyA;
  ItemInstanceProps emptyB;
  require(emptyA.empty(),"default ItemInstanceProps must be empty");
  require(ItemInstanceProps::canStack(nullptr,nullptr),"two null props must stack");
  require(ItemInstanceProps::canStack(nullptr,&emptyA),"null and empty props must stack");
  require(ItemInstanceProps::canStack(&emptyA,nullptr),"empty and null props must stack");
  require(ItemInstanceProps::canStack(&emptyA,&emptyB),"two empty props must stack");

  ItemInstanceProps warm;
  warm.temperature = 65.0f;
  ItemInstanceProps cold;
  cold.temperature = 15.0f;
  ItemInstanceProps warmCopy;
  warmCopy.temperature = 65.0f;

  require(!ItemInstanceProps::canStack(nullptr,&warm),"warm item must not stack with plain item");
  require(!ItemInstanceProps::canStack(&warm,&cold),"warm and cold items must not stack");
  require(ItemInstanceProps::canStack(&warm,&warmCopy),"equal-temperature items must stack");
  require(warm.stackKey(42)==warmCopy.stackKey(42),"equal props must produce equal stackKey");
  require(warm.stackKey(42)!=cold.stackKey(42),"differing props must produce distinct stackKey");
  require(warm.stackKey(42)!=warm.stackKey(43),"differing archetype must produce distinct stackKey");

  // Signed zero normalization for both float fields and custom double values.
  ItemInstanceProps posZero;
  posZero.quality = 0.0f;
  posZero.custom["temp_delta"] = 0.0;
  ItemInstanceProps negZero;
  negZero.quality = -0.0f;
  negZero.custom["temp_delta"] = -0.0;
  require(ItemInstanceProps::canStack(&posZero,&negZero),"-0.0 and +0.0 must canonicalize equally");
  require(posZero.stackKey(7)==negZero.stackKey(7),"-0.0 and +0.0 must hash equally");

  // Unique items never stack, even with identical fields.
  ItemInstanceProps uniqA;
  uniqA.quality = 1.0f;
  uniqA.unique  = true;
  ItemInstanceProps uniqB = uniqA;
  require(!ItemInstanceProps::canStack(&uniqA,&uniqB),"unique items must never stack");

  // Rich props + all 4 variant types + tooltip + binary SaveArchive round-trip.
  ItemInstanceProps rich;
  rich.quality       = 0.85f;
  rich.durability    = 42.0f;
  rich.maxDurability = 100.0f;
  rich.temperature   = 320.5f;
  rich.unique        = false;
  rich.custom["alloy"]   = std::string("magic_ore");
  rich.custom["forged"]  = true;
  rich.custom["heat_k"]  = 593.65;
  rich.custom["strikes"] = int64_t(12);

  const auto rows = rich.tooltipRows();
  require(rows.size()==3 &&
          rows[0].first=="Quality" && rows[0].second=="85%" &&
          rows[1].first=="Durability" && rows[1].second=="42 / 100" &&
          rows[2].first=="Temperature",
          "tooltipRows must format built-in quality, durability, and temperature");

  std::vector<uint8_t> bytes;
  {
    Tempest::MemWriter wr{bytes};
    SaveArchive out{wr};
    out.setEntry("props");
    rich.save(out);
  }
  {
    Tempest::MemReader rd{bytes};
    SaveArchive in{rd};
    require(in.setEntry("props"),"props entry must exist");
    const auto restored = ItemInstanceProps::load(in);
    require(rich.equalsCanonical(restored),"SaveArchive round-trip must preserve ItemInstanceProps exactly");
    require(rich.stackKey(99)==restored.stackKey(99),"restored stackKey must match original");
  }
  std::puts("PASS: ItemInstanceProps");
  }

void testLoadOrderAndValidation() {
  using modkit::ContentRegistry;
  using modkit::ModManifest;

  require(ContentRegistry::isValidNamespace("smithing"),"lowercase namespace must be valid");
  require(ContentRegistry::isValidNamespace("mod_a2"),"alphanumeric+underscore namespace must be valid");
  require(!ContentRegistry::isValidNamespace("BadNS"),"uppercase namespace must be rejected");
  require(!ContentRegistry::isValidNamespace("2bad"),"digit-leading namespace must be rejected");
  require(ContentRegistry::isValidCanonicalId("smithing:refined_ore_blade"),"canonical id must be valid");
  require(!ContentRegistry::isValidCanonicalId("no_colon"),"missing colon must be rejected");
  require(ContentRegistry::namespaceOf("smithing:blade")=="smithing","namespaceOf must extract prefix");

  ModManifest base;
  base.id = "base_gothic";
  base.version = "1.0.0";

  ModManifest zMod;
  zMod.id = "zeta_forge";
  zMod.version = "1.0.0";
  zMod.dependencies = {"alpha_ore"};

  ModManifest aMod;
  aMod.id = "alpha_ore";
  aMod.version = "1.0.0";

  ModManifest bMod;
  bMod.id = "beta_ui";
  bMod.version = "1.0.0";
  bMod.loadAfter = {"zeta_forge"};

  std::vector<ModManifest> ordered;
  std::string err;
  require(ContentRegistry::resolveLoadOrder({bMod,zMod,aMod,base},ordered,err),
          "valid dependency graph must resolve");
  require(ordered.size()==4 &&
          ordered[0].id=="base_gothic" &&
          ordered[1].id=="alpha_ore" &&
          ordered[2].id=="zeta_forge" &&
          ordered[3].id=="beta_ui",
          "load order must place base_gothic first and honor deps + loadAfter");

  // Cycle detection.
  ModManifest c1; c1.id = "cyc_a"; c1.version = "1.0.0"; c1.dependencies = {"cyc_b"};
  ModManifest c2; c2.id = "cyc_b"; c2.version = "1.0.0"; c2.dependencies = {"cyc_a"};
  require(!ContentRegistry::resolveLoadOrder({c1,c2},ordered,err),"cycle must be rejected");

  // Conflict detection.
  ModManifest k1; k1.id = "mod_one"; k1.version = "1.0.0"; k1.conflicts = {"mod_two"};
  ModManifest k2; k2.id = "mod_two"; k2.version = "1.0.0";
  require(!ContentRegistry::resolveLoadOrder({k1,k2},ordered,err),"conflicting mods must be rejected");

  std::puts("PASS: ContentRegistry load order & validation");
  }

void testDataStageAndSaveRemap() {
  using modkit::ContentRegistry;
  using modkit::ModManifest;

  const std::string alphaJson = R"({
    "items": [
      {
        "id": "alpha:steel_ingot",
        "name": "Refined Steel Ingot",
        "base_value": 60,
        "stackable": true,
        "ai_tags": ["metal", "forging"]
      },
      {
        "id": "alpha:legacy_charm",
        "name": "Legacy Charm",
        "base_value": 25,
        "stackable": false
      }
    ],
    "recipes": [
      {
        "id": "alpha:forge_blade",
        "title": "Forge Refined Blade",
        "station_scheme": "BSANVIL",
        "ingredients": [
          {"item": "gothic:itmiswordraw", "count": 1},
          {"item": "alpha:steel_ingot", "count": 1, "min_quality": 0.5}
        ],
        "outputs": [
          {"item": "beta:fine_blade", "count": 1}
        ]
      }
    ]
  })";

  const std::string betaJson = R"({
    "items": [
      {
        "id": "beta:fine_blade",
        "name": "Fine Longsword",
        "base_value": 450,
        "damage": {"edge": 55},
        "stackable": false,
        "default_props": {
          "quality": 0.75,
          "durability": 100.0,
          "max_durability": 100.0
        }
      }
    ]
  })";

  const std::string patch1Json = R"({
    "patches": [
      {
        "target": "beta:fine_blade",
        "op": "merge",
        "set": {
          "base_value": 520,
          "description": "Tempered masterwork blade"
        }
      },
      {
        "target": "alpha:forge_blade",
        "op": "add_ingredient",
        "item": "alpha:legacy_charm",
        "count": 1
      }
    ]
  })";

  const std::string patch2Json = R"({
    "patches": [
      {
        "target": "beta:fine_blade",
        "op": "merge",
        "set": {
          "base_value": 600
        }
      }
    ]
  })";

  std::string err;
  std::vector<std::string> warnings;

  // Foreign namespace injection must fail in Pass 1.
  {
    ContentRegistry reg;
    const std::string badJson = R"({"items":[{"id":"other:hacked","name":"Bad"}]})";
    require(!reg.loadDataJson("alpha",badJson,err),"Pass 1 must reject foreign namespace");
  }

  // Registry A: load alpha first, then beta.
  ContentRegistry regA;
  ItemRecord stockRaw{};
  stockRaw.name = "Raw Steel";
  require(regA.registerStockItem("gothic:itmiswordraw",1234,stockRaw,err),"registerStockItem must succeed");
  require(regA.loadDataJson("alpha",alphaJson,err),"alpha data load must succeed");
  require(regA.loadDataJson("beta",betaJson,err),"beta data load must succeed");
  require(regA.applyPatchesJson("gamma",patch1Json,warnings,err),"patch 1 must succeed");
  require(warnings.empty(),"first patch must not warn");
  require(regA.applyPatchesJson("delta",patch2Json,warnings,err),"patch 2 must succeed");
  require(warnings.size()==1,"conflicting second patch on same field must emit warning");
  require(regA.freeze(err),"freeze must succeed when all recipe references resolve");

  const auto* bladeA = regA.findItem("beta:fine_blade");
  require(bladeA!=nullptr && bladeA->record.value==600,"last patch in load order must win");
  require(bladeA->defaultProps.has_value() && bladeA->defaultProps->quality.value()==0.75f,
          "default_props must be loaded");
  require(!bladeA->stackable,"stackable=false must be preserved");
  require(regA.recipesForStation("bsanvil").size()==1,"recipesForStation must match case-insensitively");

  const auto alphaRuntimeA = regA.runtimeIdOfItem("alpha:steel_ingot");
  const auto betaRuntimeA  = regA.runtimeIdOfItem("beta:fine_blade");
  const auto legacyRuntimeA = regA.runtimeIdOfItem("alpha:legacy_charm");
  require(alphaRuntimeA && betaRuntimeA && legacyRuntimeA,"all items must have runtime IDs");
  require(*alphaRuntimeA==ContentRegistry::ModItemIdBase,"first registered mod item starts at ModItemIdBase");

  ModManifest mAlpha; mAlpha.id = "alpha"; mAlpha.version = "1.0.0";
  ModManifest mBeta;  mBeta.id  = "beta";  mBeta.version  = "1.0.0";
  const std::string savedManifest = regA.serializeManifestJson({mAlpha,mBeta});

  // Registry B: reversed load order (beta before alpha) AND alpha:legacy_charm removed!
  const std::string alphaTrimmedJson = R"({
    "items": [
      {
        "id": "alpha:steel_ingot",
        "name": "Refined Steel Ingot",
        "value": 60
      }
    ]
  })";

  ContentRegistry regB;
  require(regB.registerStockItem("gothic:itmiswordraw",1234,stockRaw,err),"stock item in regB");
  require(regB.loadDataJson("beta",betaJson,err),"beta loaded first in regB");
  require(regB.loadDataJson("alpha",alphaTrimmedJson,err),"trimmed alpha loaded second in regB");
  require(regB.freeze(err),"regB freeze must succeed");

  const auto alphaRuntimeB = regB.runtimeIdOfItem("alpha:steel_ingot");
  const auto betaRuntimeB  = regB.runtimeIdOfItem("beta:fine_blade");
  require(alphaRuntimeB && betaRuntimeB,"items present in regB");
  require(*alphaRuntimeA!=*alphaRuntimeB && *betaRuntimeA!=*betaRuntimeB,
          "swapped load order must assign different runtime IDs");

  std::unordered_map<uint32_t,uint32_t> remap;
  std::vector<std::string> missing;
  require(regB.buildSaveRemap(savedManifest,remap,missing,err),"buildSaveRemap must succeed");
  require(remap.at(*alphaRuntimeA)==*alphaRuntimeB,"alpha:steel_ingot must remap across load-order shift");
  require(remap.at(*betaRuntimeA)==*betaRuntimeB,"beta:fine_blade must remap across load-order shift");
  require(remap.at(1234)==1234,"stock item runtime ID must remain stable");
  require(missing.size()==1 && missing[0]=="alpha:legacy_charm",
          "removed mod item must be reported in missingArchetypes");

  std::puts("PASS: ContentRegistry Data Stage & Save Remap");
  }

void testSaveHeaderCompatibility() {
  require(SaveArchive::Current==58 && SaveArchive::MinVersion==57,
          "SaveArchive must target v58 with v57 backward compatibility");

  const auto roundTrip = [](uint16_t ver) {
    std::vector<uint8_t> bytes;
    {
      Tempest::MemWriter wr{bytes};
      Serialize out{wr};
      SaveGameHeader hdr;
      hdr.version   = ver;
      hdr.name      = "Test";
      hdr.world     = "NEWWORLD.ZEN";
      hdr.isGothic2 = 2;
      out.setEntry("header");
      out.write(hdr);
    }
    Tempest::MemReader rd{bytes};
    Serialize in{rd};
    require(in.setEntry("header"),"header entry must exist");
    SaveGameHeader loaded;
    in.read(loaded);
    return loaded;
    };

  require(!roundTrip(56).isCompatible(),"v56 must be rejected");
  require(roundTrip(57).isCompatible(),"v57 must remain backward-compatible");
  require(roundTrip(58).isCompatible(),"v58 must be compatible");
  require(!roundTrip(59).isCompatible(),"v59 must be rejected");

  std::puts("PASS: SaveGameHeader v57/v58 compatibility");
  }

void testLiveWorldPhase1(const std::filesystem::path& caseDir) {
  require(caseDir.is_absolute(),"--world case directory must be absolute");
  const auto* game = std::getenv("GOTHIC_DIR");
  require(game!=nullptr && *game!='\0',"GOTHIC_DIR must point to a game settings directory");
  setenv("GOTHIC_AI","0",1);

  const char* args[] = {"gothic-modkit-phase1-contract","-g",game};
  CommandLine commandLine(3,args);
  Resources   resources;
  Gothic      gothic;
  gothic.isNpcInDialogFn = [](const Npc*) { return false; };
  gothic.setupGlobalScripts();
  GameMusic music;
  std::filesystem::create_directories(caseDir);

  gothic.setGame(std::make_unique<GameSession>("NEWWORLD.ZEN"));
  auto* world = gothic.world();
  require(world!=nullptr && world->player()!=nullptr,"NEWWORLD.ZEN must load with hero");

  auto* hero = world->player();
  const auto xardasSym = world->script().storyNpcId("NONE_100_XARDAS");
  auto* xardas = xardasSym ? world->findNpcByInstance(*xardasSym) : nullptr;
  require(xardas!=nullptr,"Xardas must exist in NEWWORLD.ZEN");

  // O(1) EntityId lookup for NPCs, Interactives, and WorldItems.
  const auto heroEntId   = hero->entityId();
  const auto xardasEntId = xardas->entityId();
  require(heroEntId.valid() && xardasEntId.valid() && heroEntId!=xardasEntId,
          "hero and Xardas must have distinct valid EntityIds");
  require(world->findNpcByEntityId(heroEntId)==hero,"O(1) findNpcByEntityId must resolve hero");
  require(world->findNpcByEntityId(xardasEntId)==xardas,"O(1) findNpcByEntityId must resolve Xardas");

  Interactive* sampleMob = world->mobsiById(0);
  require(sampleMob!=nullptr && sampleMob->entityId().valid(),"first Interactive must have valid EntityId");
  const auto mobEntId = sampleMob->entityId();
  require(world->findInteractiveByEntityId(mobEntId)==sampleMob,
          "O(1) findInteractiveByEntityId must resolve interactive");

  const auto swordSym = world->rules().findSymbolIndex("ItMw_1h_VLK_Sword");
  require(swordSym!=size_t(-1),"ItMw_1h_VLK_Sword must exist in base catalog");

  auto* worldSword = world->addItem(swordSym,hero->position());
  require(worldSword!=nullptr && worldSword->entityId().valid() && worldSword->uid().valid(),
          "spawned world item must have valid EntityId and ItemUid");
  modkit::ItemInstanceProps worldProps;
  worldProps.durability    = 73.0f;
  worldProps.maxDurability = 100.0f;
  worldSword->setProps(worldProps);
  const auto worldItemEntId = worldSword->entityId();
  const auto worldItemUid   = worldSword->uid();
  require(world->findItemByEntityId(worldItemEntId)==worldSword,
          "O(1) findItemByEntityId must resolve world item");

  // Clear any existing swords of this class on hero & Xardas for clean assertions.
  if(const auto existing = hero->itemCount(swordSym); existing>0)
    hero->delItem(swordSym,uint32_t(existing));
  if(const auto existing = xardas->itemCount(swordSym); existing>0)
    xardas->delItem(swordSym,uint32_t(existing));

  // Two stacks with differing ItemInstanceProps MUST NOT merge; identical props MUST merge.
  modkit::ItemInstanceProps hotProps;
  hotProps.quality        = 0.90f;
  hotProps.temperature    = 450.0f;
  hotProps.custom["mod"]  = std::string("ember_smithing");

  modkit::ItemInstanceProps coldProps;
  coldProps.quality       = 0.50f;
  coldProps.temperature   = 20.0f;

  auto hotA = std::make_unique<Item>(*world,swordSym,Item::T_Inventory);
  hotA->setCount(1);
  hotA->setProps(hotProps);
  const auto hotUid = hotA->uid();
  auto* addedHotA = hero->addItem(std::move(hotA));
  require(addedHotA!=nullptr && addedHotA->uid()==hotUid,"hot stack A must keep its ItemUid");

  auto coldB = std::make_unique<Item>(*world,swordSym,Item::T_Inventory);
  coldB->setCount(1);
  coldB->setProps(coldProps);
  const auto coldUid = coldB->uid();
  auto* addedColdB = hero->addItem(std::move(coldB));
  require(addedColdB!=nullptr && addedColdB!=addedHotA && addedColdB->uid()==coldUid,
          "cold stack B must stay separate from hot stack A");

  auto hotC = std::make_unique<Item>(*world,swordSym,Item::T_Inventory);
  hotC->setCount(1);
  hotC->setProps(hotProps);
  auto* mergedHot = hero->addItem(std::move(hotC));
  require(mergedHot==addedHotA && mergedHot->count()==2,
          "identical-props stack C must merge into hot stack A (count==2) and return surviving pointer");
  require(hero->itemCount(swordSym)==3,"hero->itemCount must sum across all distinct stacks (2 + 1 == 3)");

  // Partial transferByUid: move 1 of 2 hot blades from hero to Xardas.
  xardas->addItemByUid(hotUid,*hero,1);
  require(hero->itemCountByUid(hotUid)==1,"hero hot stack must decrement to 1");
  require(hero->itemCountByUid(coldUid)==1,"hero cold stack must stay untouched");
  require(hero->itemCount(swordSym)==2,"hero total sword count must be 2");
  require(xardas->itemCount(swordSym)==1,"Xardas must receive 1 hot blade");

  auto* xardasBlade = xardas->getItem(swordSym);
  require(xardasBlade!=nullptr && xardasBlade->uid().valid() && xardasBlade->uid()!=hotUid,
          "split transfer must allocate a fresh ItemUid for Xardas");
  require(xardasBlade->props()!=nullptr && xardasBlade->props()->equalsCanonical(hotProps),
          "split transfer must clone ItemInstanceProps onto Xardas's blade");
  const auto xardasBladeUid = xardasBlade->uid();

  // Save full session to v58 and inspect entries.
  std::vector<uint8_t> saveBytes;
  {
    Tempest::MemWriter wr{saveBytes};
    Serialize          out{wr};
    Tempest::Pixmap    preview(1,1,Tempest::TextureFormat::RGBA8);
    std::memset(preview.data(),0,4);
    gothic.gameSession()->save(out,"Modkit Phase 1",preview);
  }
  {
    Tempest::MemReader rd{saveBytes.data(),saveBytes.size()};
    Serialize          in{rd};
    SaveGameHeader     hdr;
    require(in.setEntry("header"),"v58 save must contain header");
    in.read(hdr);
    require(hdr.version==58 && hdr.isCompatible(),"save header version must be 58");
    require(in.setEntry("game/modkit-ids"),"v58 save must contain game/modkit-ids");
    require(in.setEntry("mods/manifest.json"),"v58 save must contain mods/manifest.json");
    require(in.setEntry("worlds/NEWWORLD.ZEN/entity-ids"),"v58 save must contain worlds/NEWWORLD.ZEN/entity-ids");
  }

  // Reload v58 into a fresh GameSession and verify every ID, O(1) index, and ItemInstanceProps!
  gothic.clearGame().reset();
  {
    Tempest::MemReader rd{saveBytes.data(),saveBytes.size()};
    Serialize          in{rd};
    gothic.setGame(std::make_unique<GameSession>(in));
  }

  world  = gothic.world();
  hero   = world!=nullptr ? world->player() : nullptr;
  require(world!=nullptr && hero!=nullptr,"reloaded world and hero must be valid");
  require(hero->entityId()==heroEntId,"hero EntityId must survive v58 reload");
  require(world->findNpcByEntityId(heroEntId)==hero,"O(1) hero lookup must work after v58 reload");

  xardas = world->findNpcByEntityId(xardasEntId);
  require(xardas!=nullptr && xardas->entityId()==xardasEntId,"O(1) Xardas lookup must survive v58 reload");

  sampleMob = world->findInteractiveByEntityId(mobEntId);
  require(sampleMob!=nullptr && sampleMob->entityId()==mobEntId,
          "O(1) Interactive lookup must survive v58 reload");

  worldSword = world->findItemByEntityId(worldItemEntId);
  require(worldSword!=nullptr && worldSword->uid()==worldItemUid &&
          worldSword->props()!=nullptr && worldSword->props()->equalsCanonical(worldProps),
          "WorldItem EntityId, ItemUid, and ItemInstanceProps must survive v58 reload");

  auto* reloadedHot  = hero->getItemByUid(hotUid);
  auto* reloadedCold = hero->getItemByUid(coldUid);
  require(reloadedHot!=nullptr && reloadedCold!=nullptr && reloadedHot!=reloadedCold,
          "both distinct stacks must survive v58 reload with their original ItemUids");
  require(reloadedHot->count()==1 && reloadedHot->props()!=nullptr &&
          reloadedHot->props()->equalsCanonical(hotProps),
          "hot stack props must survive v58 reload");
  require(reloadedCold->count()==1 && reloadedCold->props()!=nullptr &&
          reloadedCold->props()->equalsCanonical(coldProps),
          "cold stack props must survive v58 reload");

  auto* reloadedXardasBlade = xardas->getItemByUid(xardasBladeUid);
  require(reloadedXardasBlade!=nullptr && reloadedXardasBlade->count()==1 &&
          reloadedXardasBlade->props()!=nullptr &&
          reloadedXardasBlade->props()->equalsCanonical(hotProps),
          "Xardas split stack ItemUid and props must survive v58 reload");

  // Generator must keep allocating strictly monotonic IDs past all restored IDs.
  const auto freshEnt = world->allocEntityId();
  const auto freshItm = world->allocItemUid();
  require(freshEnt.raw > worldItemEntId.raw && freshEnt.raw > mobEntId.raw,
          "restored IdGenerator must allocate EntityIds strictly above existing IDs");
  require(freshItm.raw > xardasBladeUid.raw && freshItm.raw > coldUid.raw,
          "restored IdGenerator must allocate ItemUids strictly above existing IDs");

  gothic.clearGame().reset();
  std::puts("PASS: Live NEWWORLD.ZEN Phase 1 contract (stacking, transferByUid, O(1) EntityId, v58 save/reload)");
  }

} // namespace

int main(int argc, const char** argv) {
  testIdGenerator();
  testItemInstanceProps();
  testLoadOrderAndValidation();
  testDataStageAndSaveRemap();
  testSaveHeaderCompatibility();
  if(argc==3 && std::string_view(argv[1])=="--world") {
    testLiveWorldPhase1(argv[2]);
    }
  std::puts("ALL PHASE 1 MODKIT CONTRACT TESTS PASSED");
  return 0;
  }

#include "gamescript.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <optional>
#include <span>

#include <Tempest/Log>

#include "basestore.h"
#include "game/definitions/basescripts.h"
#include "game/definitions/dialoginfodata.h"
#include "game/definitions/nativebasecatalog.h"
#include "game/serialize.h"
#include "resources.h"
#include "world/objects/item.h"
#include "world/objects/npc.h"

namespace {

int32_t fixedTalentLearnCost(int32_t talent, int32_t skill) {
  // Tables indexed by the verified NotR skill IDs.
  static constexpr uint8_t smith[] = {2,4,4,6,6,8,8,10,10,4,6,8,10};
  static constexpr uint8_t alchemy[] = {1,3,5,1,3,5,5,20,20,10,10,20,0,8,8};
  static constexpr uint8_t trophies[] = {3,3,5,3,3,1,1,1,1,3,1,3,3,3};
  static constexpr uint8_t languages[] = {5,10,15};
  static constexpr uint8_t wisp[] = {0,1,2,3,4,5,5};
  static constexpr uint8_t runes[] = {
     1, 3, 3, 5, 5,10,10, 5, 5, 5, //  0–9
     5, 5, 5, 5, 5, 5, 5, 5, 1, 5, // 10–19
     5, 3, 3,10, 3, 5, 5, 5, 5, 5, // 20–29
    10,10, 5,10,10,15,10,10,15,20, // 30–39
    20,10,20,20,20,20,20, 5, 5, 5, // 40–49
     5, 5, 5, 5, 5, 5, 5, 5, 5, 5, // 50–59
    15, 5, 5, 5, 5, 5, 5, 5, 5, 5, // 60–69
     5, 5,10, 5, 5,10              // 70–75
    };
  const auto price = [index=size_t(skill)](std::span<const uint8_t> table, int32_t fallback = 0) {
    return index<table.size() ? table[index] : fallback;
    };
  switch(talent) {
    case TALENT_MAGE:             return 1<=skill && skill<=6 ? 5 : 0;
    case TALENT_ACROBAT:
    case TALENT_SNEAK:            return 5;
    case TALENT_PICKPOCKET:
    case TALENT_PICKLOCK:         return 10;
    case TALENT_SMITH:            return price(smith);
    case TALENT_ALCHEMY:          return price(alchemy);
    case TALENT_TAKEANIMALTROPHY: return price(trophies);
    case TALENT_FOREIGNLANGUAGE:  return price(languages);
    case TALENT_WISPDETECTOR:     return price(wisp);
    case TALENT_RUNES:            return price(runes,5);
    default:                      return 0;
    }
  }

} // namespace

auto GameScript::createNpc(Npc& owner, size_t instance, std::string_view waypoint, uint32_t& kind) -> std::shared_ptr<NpcRecord> {
  auto npc = std::make_shared<daedalus::INpc>();
  owner.hnpc = npc;
  npc->user_ptr = &owner;
  npc->id = int32_t(instance & 0x7FFFFFFF);
  npc->wp = std::string(waypoint);
  kind = npc->symbol_index();
  if(instance!=size_t(-1))
    initializeInstanceNpc(npc,instance);
  kind = npc->symbol_index();
  return npc;
  }

uint32_t GameScript::initializeInstanceNpc(const std::shared_ptr<NpcRecord>& record, size_t instance) {
  auto npc = std::static_pointer_cast<NpcInstance>(record);
  initializeInstanceNpc(npc,instance);
  return npc->symbol_index();
  }

auto GameScript::loadNpc(Serialize& fin, Npc* owner, uint32_t& kind) -> std::shared_ptr<NpcRecord> {
  auto npc = std::make_shared<daedalus::INpc>();
  if(owner!=nullptr)
    owner->hnpc = npc;
  npc->user_ptr = owner;
  uint32_t symbol = 0;
  fin.read(symbol);
  auto* alias = ownedInstance(symbol);
  if(alias==nullptr || !npcTemplateId(symbol))
    throw BaseStore::Error("Unknown NPC identity in save: "+std::to_string(symbol));
  npc->setRecordIdentity(symbol);
  *alias = npc;
  kind = npc->symbol_index();
  fin.readNpc(*npc);
  return npc;
  }

void GameScript::bindNpc(const std::shared_ptr<NpcRecord>& record) {
  auto npc = std::static_pointer_cast<NpcInstance>(record);
  if(auto* alias = ownedInstance(npc->symbol_index()))
    *alias = npc;
  }

auto GameScript::copyNpc(const std::shared_ptr<NpcRecord>& record) -> std::shared_ptr<NpcRecord> {
  return std::make_shared<daedalus::INpc>(*std::static_pointer_cast<daedalus::INpc>(record));
  }

void GameScript::restoreNpc(const std::shared_ptr<NpcRecord>& dst, const std::shared_ptr<NpcRecord>& src, Npc& owner) {
  auto npc = std::static_pointer_cast<daedalus::INpc>(dst);
  *npc = *std::static_pointer_cast<daedalus::INpc>(src);
  npc->user_ptr = &owner;
  }

void GameScript::detachNpc(const std::shared_ptr<NpcRecord>& record, Npc& owner) {
  auto npc = std::static_pointer_cast<daedalus::INpc>(record);
  if(npc && npc->user_ptr==&owner)
    npc->user_ptr = nullptr;
  }

Npc* GameScript::findNpc(NpcRecord* record) {
  return record==nullptr ? nullptr : static_cast<Npc*>(static_cast<daedalus::INpc*>(record)->user_ptr);
  }

void GameScript::loadNativeAmbientInfos() {
  using Actor = std::shared_ptr<NpcInstance>;
  constexpr uint32_t ambient = 1u<<uint32_t(NpcType::G2_AMBIENT);
  constexpr uint32_t oldCamp = 1u<<uint32_t(NpcType::G2_OC_AMBIENT);
  constexpr uint32_t banditCamp = 1u<<uint32_t(NpcType::G2_BL_AMBIENT);
  constexpr uint32_t valley = 1u<<uint32_t(NpcType::G2_TAL_AMBIENT);
  constexpr int32_t anyVoice = -1;
  static constexpr struct Selector {
    int32_t guild;
    uint32_t types;
    int32_t voice;
    std::string_view suffix;
    } selectors[] = {
    {GIL_VLK, ambient, 1,  "VLK_1"},
    {GIL_VLK, ambient, 6,  "VLK_6"},
    {GIL_VLK, ambient, 8,  "VLK_8"},
    {GIL_VLK, ambient, 16, "VLK_16"},
    {GIL_VLK, ambient, 17, "VLK_17"},
    {GIL_MIL, ambient, 6, "MIL_6"},
    {GIL_MIL, ambient, 7, "MIL_7"},
    {GIL_MIL, oldCamp, 1, "OCVLK_1"},
    {GIL_MIL, oldCamp, 6, "OCVLK_6"},
    {GIL_PAL, ambient, 4,  "PAL_4"},
    {GIL_PAL, ambient, 9,  "PAL_9"},
    {GIL_PAL, ambient, 12, "PAL_12"},
    {GIL_PAL, oldCamp, 4, "OCPAL_4"},
    {GIL_PAL, oldCamp, 9, "OCPAL_9"},
    {GIL_BAU, ambient, 1,  "BAU_1"},
    {GIL_BAU, ambient, 7,  "BAU_7"},
    {GIL_BAU, ambient, 13, "BAU_13"},
    {GIL_BAU, ambient, 16, "BAU_16"},
    {GIL_SLD, ambient, 6, "SLD_6"},
    {GIL_SLD, ambient, 7, "SLD_7"},
    {GIL_NOV, ambient, 3, "NOV_3"},
    {GIL_NOV, ambient, 8, "NOV_8"},
    {GIL_PIR, ambient, 6, "ADDON_PIR_6"},
    {GIL_PIR, ambient, 7, "ADDON_PIR_7"},
    {GIL_OUT, ambient, 1,  "OUT_1"},
    {GIL_OUT, ambient, 7,  "OUT_7"},
    {GIL_OUT, ambient, 13, "OUT_13"},
    {GIL_OUT, oldCamp, 4, "OWPAL_4"},
    {GIL_STRF, ambient|oldCamp, 1,  "STRF_1"},
    {GIL_STRF, ambient|oldCamp, 13, "STRF_13"},
    {GIL_STRF, banditCamp, anyVoice,"ADDON_SKLAVEN_3"},
    {GIL_BDT, ambient|oldCamp, 1,  "BDT_1"},
    {GIL_BDT, ambient|oldCamp, 13, "BDT_13"},
    {GIL_BDT, banditCamp, 1,  "ADDON_BL_BDT_1"},
    {GIL_BDT, banditCamp, 13, "ADDON_BL_BDT_13"},
    {GIL_BDT, valley, 1,  "ADDON_TAL_BDT_1"},
    {GIL_BDT, valley, 13, "ADDON_TAL_BDT_13"},
    {GIL_DMT, ambient|oldCamp, 19, {}},
    };
  const auto assign = [](const Actor& actor, const auto& apply) {
    if(actor==nullptr || uint32_t(actor->type)>=32u)
      return;
    const auto typeBit = 1u<<uint32_t(actor->type);
    for(size_t i=0; i<std::size(selectors); ++i) {
      const auto& selector = selectors[i];
      if(actor->guild==selector.guild && (selector.types&typeBit)!=0 &&
         (selector.voice==anyVoice || actor->voice==selector.voice))
        apply(i);
      }
    };
  const auto ambientData = DialogInfoData::loadAmbient(Resources::baseDir());
  const auto* catalog = baseNativeCatalog();
  if(!ambientData)
    throw BaseStore::Error("native base is missing stock ambient dialog bindings");
  struct Binding {
    ScriptVars::Object* savedActor = nullptr;
    std::vector<uint32_t> topics;
    };
  std::array<Binding,std::size(selectors)> bindings;
  for(size_t i=0; i<std::size(selectors); ++i) {
    if(selectors[i].suffix.empty())
      continue;
    const auto name = std::string("B_ASSIGNAMBIENTINFOS_").append(selectors[i].suffix);
    const auto found = ambientData->find(name);
    if(found==ambientData->end())
      throw BaseStore::Error("missing stock ambient dialog binding: "+name);
    const auto alias = runtimeState.id(name+".SLF");
    auto& binding = bindings[i];
    binding.savedActor = alias==found->second.actor ? storyOwnedInstance(name+".SLF") : nullptr;
    if(binding.savedActor==nullptr || std::ranges::any_of(found->second.topics,[&](uint32_t id) {
          return NativeBaseCatalog::findIdentity(catalog->identities.dialogs,id)==nullptr;
          }))
      throw BaseStore::Error("invalid stock ambient dialog binding: "+name);
    binding.topics = found->second.topics;
    }
  auto* savedActor = &storyActorSlot("B_ASSIGNAMBIENTINFOS.SLF");
  nativeExternals.insert_or_assign("B_ASSIGNAMBIENTINFOS",storygraph::makeNativeInternal(std::function<void(Actor)>{
    [this,savedActor,assign,bindings=std::move(bindings)](const Actor& actor) {
      *savedActor = actor;
      assign(actor,[&](size_t i) {
        const auto& binding = bindings[i];
        if(binding.savedActor==nullptr) {
          assignDementorTalk(actor);
          return;
          }
        *binding.savedActor = actor;
        const auto owner = hlp_getinstanceid(actor);
        for(auto id : binding.topics) {
          auto* info = findInfo(id);
          if(info==nullptr)
            throw std::runtime_error("missing loaded stock ambient dialog "+std::to_string(id));
          info->npc = owner;
          }
        });
      }}));
  }

void GameScript::readDialogInfos() {
  const auto* catalog = baseNativeCatalog();
  const auto rows = DialogInfoData::load(Resources::baseDir());
  if(!rows)
    throw BaseStore::Error("native base is missing stock dialog definitions");

  using Info = DialogInstance;
  // Member IDs in the verified stock program that supplies dialog-infos.json.
  constexpr uint32_t firstMember = 1587;
  static constexpr std::array<int32_t Info::*,8> members = {
    &Info::npc, &Info::nr, &Info::important, &Info::condition, &Info::information,
    nullptr, // DESCRIPTION holds text or a lesson expression.
    &Info::trade, &Info::permanent
    };

  const auto setLessonInt = [&](std::string_view name, int32_t value) {
    if(!setGlobalIntSymbol(name,value))
      throw BaseStore::Error("native dialog definition needs lesson state "+std::string(name));
    };
  const auto lessonString = [&](std::string_view name) -> std::string& {
    auto* slot = std::get_if<ScriptVars::Array<std::string>>(runtimeState.find(name));
    if(slot==nullptr || slot->values.size()!=1)
      throw BaseStore::Error("native dialog definition needs lesson state "+std::string(name));
    return slot->values.front();
    };
  const auto lessonCaption = [&](const DialogInfoData::Lesson& lesson) -> std::string_view {
    setLessonInt("B_GETLEARNCOSTTALENT.SKILL",lesson.skill);
    setLessonInt("B_GETLEARNCOSTTALENT.TALENT",lesson.talent);
    storyActorSlot("B_GETLEARNCOSTTALENT.OTH") = storyActorSlot("OTHER");

    const auto cost = fixedTalentLearnCost(lesson.talent,lesson.skill);
    setLessonInt("B_GETLEARNCOSTTALENT.KOSTEN",cost);
    setLessonInt("B_BUILDLEARNSTRING.KOSTEN",cost);

    auto& title = lessonString("B_BUILDLEARNSTRING.TEXT");
    auto& result = lessonString("B_BUILDLEARNSTRING.CONCATTEXT");
    const auto costText = stringSymbol("PRINT_KOSTEN",{});
    const auto lp = stringSymbol("PRINT_LP",{});
    if(costText.data()==nullptr || lp.data()==nullptr)
      throw BaseStore::Error("native dialog definition needs the stock lesson caption constants");
    title = lesson.title;
    result = title;
    result.append(costText).append(std::to_string(cost)).append(lp);
    return result;
    };

  dialogsInfo.reserve(rows->size());
  const auto read = [&](uint32_t id, const std::string& name, const std::shared_ptr<Info>& info) {
    const auto fields = rows->find(id);
    if(fields==rows->end())
      throw BaseStore::Error("dialog infos: missing instance "+name);
    for(const auto& field : fields->second) {
      const auto member = field.member-firstMember;
      if(member>=members.size())
        throw BaseStore::Error("dialog infos: invalid member for "+name);
      if(const auto number = members[member]) {
        const auto* value = std::get_if<int32_t>(&field.value);
        if(value==nullptr)
          throw BaseStore::Error("dialog infos: invalid numeric field for "+name);
        (*info).*number = *value;
        } else if(const auto* title = std::get_if<std::string>(&field.value)) {
        info->description = *title;
        } else if(const auto* lesson = std::get_if<DialogInfoData::Lesson>(&field.value)) {
        info->description = lessonCaption(*lesson);
        } else {
        throw BaseStore::Error("dialog infos: invalid description for "+name);
        }
      }
    dialogsInfo.push_back(info);
    };
  for(const auto& dialog : catalog->identities.dialogs) {
    auto* alias = ownedInstance(dialog.id);
    if(alias==nullptr)
      throw BaseStore::Error("dialog infos: invalid native identity "+dialog.name);
    auto info = std::make_shared<Info>();
    info->setRecordIdentity(dialog.id);
    read(dialog.id,dialog.name,info);
    *alias = std::move(info);
    }
  if(dialogsInfo.size()!=rows->size())
    throw BaseStore::Error("dialog infos: unexpected instance in stock records");
  }

void GameScript::setNpcInfoKnown(const NpcRecord& npc, const daedalus::IInfo& info) {
  const auto actor = static_cast<const daedalus::INpc&>(npc).symbol_index();
  if(actor!=uint32_t(-1) && info.symbol_index()!=uint32_t(-1))
    dlgKnownInfos.emplace(actor,info.symbol_index());
  }

bool GameScript::markNpcInfoKnown(const Npc& npc, std::string_view name) {
  const auto* info = storyDialog(name);
  if(info==nullptr)
    return false;
  setNpcInfoKnown(*npc.handlePtr(),*info);
  return true;
  }

bool GameScript::doesNpcKnowInfo(const NpcRecord& npc, size_t infoInstance) const {
  const auto actor = static_cast<const daedalus::INpc&>(npc).symbol_index();
  return dlgKnownInfos.contains({actor,infoInstance});
  }

const DialogInstance* GameScript::storyDialog(std::string_view name) const {
  const auto* dialog = NativeBaseCatalog::findIdentity(baseNativeCatalog()->identities.dialogs,name);
  if(dialog==nullptr)
    return nullptr;
  const auto* alias = std::get_if<ScriptVars::Object>(runtimeState.find(dialog->id));
  return alias!=nullptr ? dynamic_cast<const DialogInstance*>(alias->get()) : nullptr;
  }

std::string_view GameScript::storyDialogName(uint32_t id) const {
  const auto* dialog = NativeBaseCatalog::findIdentity(baseNativeCatalog()->identities.dialogs,id);
  return dialog!=nullptr ? std::string_view(dialog->name) : std::string_view{};
  }

std::optional<uint32_t> GameScript::storyDialogFunction(std::string_view name, bool condition) const {
  const auto* info = storyDialog(name);
  if(info==nullptr)
    return std::nullopt;
  const auto function = condition ? info->condition : info->information;
  return function>0 ? std::optional<uint32_t>(uint32_t(function)) : std::nullopt;
  }

auto GameScript::createItem(Item& owner, size_t instance, uint32_t& kind) -> std::shared_ptr<ItemRecord> {
  auto item = std::make_shared<daedalus::IItem>();
  owner.hitem = item;
  item->user_ptr = &owner;
  kind = uint32_t(instance);
  initializeInstanceItem(item,instance);
  kind = item->symbol_index();
  return item;
  }

auto GameScript::createItem(size_t instance) -> std::shared_ptr<ItemRecord> {
  auto item = std::make_shared<daedalus::IItem>();
  initializeInstanceItem(item,instance);
  return item;
  }

auto GameScript::loadItem(Serialize& fin, Item& owner, uint32_t& kind) -> std::shared_ptr<ItemRecord> {
  auto item = std::make_shared<daedalus::IItem>();
  owner.hitem = item;
  item->user_ptr = &owner;
  uint32_t symbol = 0;
  fin.read(symbol);
  bindPreparedItem(item,symbol);
  kind = item->symbol_index();
  fin.readItem(*item);
  return item;
  }

void GameScript::attachItem(const std::shared_ptr<ItemRecord>& record, Item& owner) {
  std::static_pointer_cast<daedalus::IItem>(record)->user_ptr = &owner;
  }

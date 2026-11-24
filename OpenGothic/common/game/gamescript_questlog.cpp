#include "gamescript.h"

#include "basestore.h"
#include "game/definitions/questclosuredata.h"
#include "resources.h"

#include <algorithm>
#include <bit>

namespace {
void printQuestNotice(daedalus::DaedalusVm& vm, daedalus::DaedalusSymbol& print,
                      daedalus::DaedalusSymbol* text, daedalus::DaedalusSymbol* y, daedalus::DaedalusSymbol* font) {
  vm.call_bound_native(print,[&] {
    vm.push_reference(text);
    vm.push_int(-1);
    vm.push_reference(y);
    vm.push_reference(font);
    vm.push_int(2);
    },[&] { (void)vm.pop_int(); });
  }
}

void GameScript::givePlayerXp(int32_t amount) {
  struct RestoreContext {
    daedalus::DaedalusVm& vm;
    std::shared_ptr<daedalus::DaedalusInstance> caller;
    ~RestoreContext() { vm.unsafe_set_gi(std::move(caller)); }
    } restore{vm,vm.unsafe_get_gi()};
  const auto add=[](int32_t a,int32_t b) {
    return std::bit_cast<int32_t>(uint32_t(a)+uint32_t(b));
    };
  const auto hero=[&] {
    return std::dynamic_pointer_cast<daedalus::INpc>(vm.global_hero()->get_instance());
    };
  const auto print=[&](daedalus::DaedalusSymbol* text,const char* y,const char* font) {
    printQuestNotice(vm,*storySlot("PRINTSCREEN"),text,storySlot(y),storySlot(font));
    };

  auto* savedAmount=storySlot("B_GIVEPLAYERXP.ADD_XP");
  auto* text=storySlot("B_GIVEPLAYERXP.CONCATTEXT");
  savedAmount->set_int(amount);
  const auto current=hero();
  vm.unsafe_set_gi(current);
  if(current) {
    if(current->level==0)
      current->exp_next=500;
    current->exp=add(current->exp,amount);
    }

  // The original formatting calls also update the VM's temporary strings.
  text->set_string(storySlot("PRINT_XPGAINED")->get_string());
  std::string amountText;
  vm.call_bound_native(*storySlot("INTTOSTRING"),[&] { vm.push_reference(savedAmount); },
                       [&] { amountText = vm.pop_string(); });
  vm.call_bound_native(*storySlot("CONCATSTRINGS"),[&] {
    vm.push_reference(text);
    vm.push_string(amountText);
    },[&] { text->set_string(vm.pop_string()); });
  print(text,"YPOS_XPGAINED","FONT_SCREENSMALL");

  const auto levelUp=hero();
  vm.unsafe_set_gi(levelUp);
  if(!levelUp || levelUp->exp>=levelUp->exp_next) {
    if(levelUp) {
      levelUp->level=add(levelUp->level,1);
      const auto threshold=std::bit_cast<int32_t>(500u*(uint32_t(levelUp->level)+1u));
      levelUp->exp_next=add(levelUp->exp_next,threshold);
      const auto health=storySlot("HP_PER_LEVEL")->get_int();
      levelUp->attribute[ATR_HITPOINTSMAX]=add(levelUp->attribute[ATR_HITPOINTSMAX],health);
      levelUp->attribute[ATR_HITPOINTS]=add(levelUp->attribute[ATR_HITPOINTS],health);
      levelUp->lp=add(levelUp->lp,storySlot("LP_PER_LEVEL")->get_int());
      }
    print(storySlot("PRINT_LEVELUP"),"YPOS_LEVELUP","FONT_SCREEN");
    vm.call_bound_native(*storySlot("SND_PLAY"),[&] { vm.push_reference(levelUpSound); },[] {});
    }

  vm.unsafe_call_bl(storySlot("B_CHECKLOG"));
  }

void GameScript::closeQuestTopic(std::string_view topic, int32_t mission, int32_t common, int32_t endChapter) {
  auto* savedTopic=storySlot("B_CLOSETOPIC.TOPIC");
  storySlot("B_CLOSETOPIC.ENDCHAPTER")->set_int(endChapter);
  storySlot("B_CLOSETOPIC.COMMONVAR")->set_int(common);
  storySlot("B_CLOSETOPIC.MISSIONVAR")->set_int(mission);
  savedTopic->set_string(topic);

  using Status=QuestLog::Status;
  auto status=Status::Running;
  if(common==1 || mission==int32_t(Status::Success))
    status=Status::Success;
  else if(mission==int32_t(Status::Obsolete) || mission==int32_t(Status::Failed))
    status=Status(mission);
  else if((common==0 || mission==int32_t(Status::Running)) && storyChapter>=endChapter)
    status=Status::Obsolete;
  if(status!=Status::Running)
    vm.call_bound_native(*storySlot("LOG_SETTOPICSTATUS"),[&] {
      vm.push_reference(savedTopic);
      vm.push_int(int32_t(status));
      },[] {});
  }

void GameScript::loadNativeQuestLog(const std::function<bool()>& enabled) {
  levelUpSound = findSymbol("$12941");
  auto* logEntrySound = findSymbol("$12894");
  vm.override_function("B_LOGENTRY",std::function<void(std::string_view,std::string_view)>{
    [this,logEntrySound](std::string_view topic, std::string_view entry) {
      auto* savedTopic=storySlot("B_LOGENTRY.TOPIC");
      auto* savedEntry=storySlot("B_LOGENTRY.ENTRY");
      savedEntry->set_string(entry);
      savedTopic->set_string(topic);
      log_addentry(savedTopic->get_string(),savedEntry->get_string());
      printQuestNotice(vm,*storySlot("PRINTSCREEN"),storySlot("PRINT_NEWLOGENTRY"),
                       storySlot("YPOS_LOGENTRY"),storySlot("FONT_SCREENSMALL"));
      vm.call_bound_native(*storySlot("SND_PLAY"),[&] { vm.push_reference(logEntrySound); },[] {});
      }},enabled);
  vm.override_function("B_GETDAYPLUS",std::function<int32_t()>{[this] {
    const bool nextDay=wld_istime(22,0,23,59);
    return std::bit_cast<int32_t>(uint32_t(wld_getday())+uint32_t(nextDay));
    }},enabled);

  vm.override_function("B_CLOSETOPIC",std::function<void(std::string_view,int32_t,int32_t,int32_t)>{
    [this](std::string_view topic,int32_t mission,int32_t common,int32_t chapter) {
      closeQuestTopic(topic,mission,common,chapter);
      }},enabled);

  const auto base = Resources::baseDir();
  if(!std::filesystem::exists(QuestClosureData::section(base)))
    return;
  auto groups = QuestClosureData::read(base);
  using Type = daedalus::DaedalusDataType;
  auto slots = std::make_shared<std::unordered_map<uint32_t,daedalus::DaedalusSymbol*>>();
  const auto checkSymbol = [&](uint32_t id, Type type) {
    auto* symbol = findSymbol(id);
    if(symbol==nullptr || symbol->type()!=type || symbol->is_member())
      throw BaseStore::Error(QuestClosureData::section(base).string()+": invalid quest symbol "+std::to_string(id));
    slots->try_emplace(id,symbol);
    };
  for(const auto& group : groups) {
    for(const auto& row : group.rows) {
      checkSymbol(row.topic,Type::STRING);
      for(const auto& value : row.values)
        if(const auto* symbol = std::get_if<uint32_t>(&value))
          checkSymbol(*symbol,Type::INT);
      }
    if(const auto& rule = group.guildRule) {
      checkSymbol(rule->topic,Type::STRING);
      checkSymbol(rule->status,Type::INT);
      for(auto guild : rule->excluded)
        checkSymbol(guild,Type::INT);
      }
    }

  auto* close = findSymbol("B_CLOSETOPIC");
  auto* setStatus = findSymbol("LOG_SETTOPICSTATUS");
  std::array<daedalus::DaedalusSymbol*,QuestClosureData::names.size()> callbacks = {};
  for(size_t i=0;i<groups.size();++i) {
    const auto name = QuestClosureData::names[i];
    callbacks[i] = findSymbol(name);
    auto& [rows,guildRule] = groups[i];
    vm.override_function(name,std::function<void()>{[this, rows=std::move(rows), guildRule, slots, close, setStatus] {
      for(size_t i=0;i<rows.size();++i) {
        if(guildRule && i==guildRule->before) {
          const auto context=vm.global_hero()->get_instance();
          vm.unsafe_set_gi(context);
          const auto hero=std::dynamic_pointer_cast<daedalus::INpc>(context);
          const auto guild=hero ? hero->guild : 0;
          if(std::ranges::none_of(guildRule->excluded,[&](uint32_t id) { return slots->at(id)->get_int()==guild; }))
            vm.call_bound_native(*setStatus,[&] {
              vm.push_reference(slots->at(guildRule->topic));
              vm.push_reference(slots->at(guildRule->status));
              },[] {});
          }
        const auto& row=rows[i];
        vm.push_reference(slots->at(row.topic));
        for(const auto& value:row.values) {
          if(const auto* literal=std::get_if<int32_t>(&value))
            vm.push_int(*literal);
          else
            vm.push_reference(slots->at(std::get<uint32_t>(value)));
          }
        vm.unsafe_call_bl(close);
        }
      }},enabled);
    }
  vm.override_function("B_CHECKLOG",std::function<void()>{[this,callbacks] {
    for(auto* callback : callbacks)
      vm.unsafe_call_bl(callback);
    }},enabled);
  }

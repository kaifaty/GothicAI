#include "gamescript.h"
#include "world/npcactivityprobe.h"

namespace { thread_local bool scriptRandom=false; }

uint32_t GameScript::rand(uint32_t max, const Npc* actor) {
  auto* stream=&randGen;
  if(actor!=nullptr && !isolatedRandom.empty()) {
    auto it=isolatedRandom.find(actor);
    if(it!=isolatedRandom.end()) stream=&it->second;
    }
  const auto result=uint32_t((*stream)())%max;
  if(auto* probe=NpcActivityProbe::instance) probe->random(actor,max,result,scriptRandom);
  return result;
  }

int GameScript::hlp_random(int bound) {
  uint32_t mod=uint32_t(std::max(1,bound));
  const Npc* self=nullptr;
  if(!isolatedRandom.empty() && vm.global_self()!=nullptr)
    self=findNpc(vm.global_self());
  if(!NpcActivityProbe::instance) return int32_t(rand(mod,self));
  if(self==nullptr && vm.global_self()->is_instance_of<daedalus::INpc>())
    self=static_cast<Npc*>(static_cast<daedalus::INpc*>(vm.global_self()->get_instance().get())->user_ptr);
  struct Restore { bool previous=scriptRandom; ~Restore() {scriptRandom=previous;} } restore;
  scriptRandom=true;
  return int32_t(rand(mod,self));
  }

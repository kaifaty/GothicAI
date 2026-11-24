#include "gamescript_daystats.h"
#include "daedalus/classes.h"
#include "utils/jsonescape.h"

namespace {
std::string counts(const std::map<std::string,uint64_t>& values) {
  std::string out="{";
  for(const auto& [name,value]:values) {
    if(out.size()>1) out+=",";
    out+=jsonEscape(name)+":"+std::to_string(value);
    }
  return out+"}";
  }
}

DayScriptStats::DayScriptStats(const RoutineDefinitions::Library& routines) {
  for(const auto& [name,rows]:routines.routines) {
    (void)name;
    for(const auto& row:rows) ++states[row.state].rows;
    }
  }

std::shared_ptr<DayScriptStats> DayScriptStats::attach(daedalus::DaedalusVm& vm, const RoutineDefinitions::Library& routines) {
  auto stats=std::make_shared<DayScriptStats>(routines);
  for(const auto& symbol:vm.symbols()) {
    if(symbol.type()==daedalus::DaedalusDataType::FUNCTION && symbol.name().starts_with("ZS_MM_"))
      stats->states[statePhase(symbol.name()).first];
    auto* mutableSymbol=vm.find_symbol_by_index(symbol.index());
    if(mutableSymbol->is_instance_of<daedalus::IInfo>()) {
      const auto* info=static_cast<const daedalus::IInfo*>(mutableSymbol->get_instance().get());
      for(const auto function:{info->condition,info->information})
        if(const auto* fn=vm.find_symbol_by_index(uint32_t(function))) stats->dialogs.insert(fn->name());
      }
    }
  stats->callObserver=vm.add_call_observer([stats,&vm](const auto& symbol) { stats->call(symbol,vm); });
  stats->instructionObserver=vm.add_instruction_observer([stats](const auto& machine,const auto& instruction) {
    stats->instruction(instruction,machine.pc());
    });
  vm.observe_externals([stats](const auto& symbol) { stats->external(symbol); });
  return stats;
  }

void DayScriptStats::stop(daedalus::DaedalusVm& vm) {
  active=false;
  vm.remove_call_observer(callObserver);
  vm.remove_instruction_observer(instructionObserver);
  vm.observe_externals({});
  }

std::pair<std::string,std::string> DayScriptStats::statePhase(std::string_view symbol) {
  if(!symbol.starts_with("ZS_")) return {};
  if(symbol.ends_with("_LOOP")) return {std::string(symbol.substr(0,symbol.size()-5)),"loop"};
  if(symbol.ends_with("_END")) return {std::string(symbol.substr(0,symbol.size()-4)),"end"};
  return {std::string(symbol),"init"};
  }

std::string DayScriptStats::family(std::string_view name) const {
  const auto [state,phase]=statePhase(name);
  if(!state.empty()) {
    if(auto it=states.find(state); it!=states.end() && it->second.rows) return "human_routines";
    return state.starts_with("ZS_MM_") ? "beast_states" : "other_states";
    }
  if(perceptions.contains(std::string(name))) return "perceptions";
  if(dialogs.contains(std::string(name))) return "dialogs";
  if(name.starts_with("B_ASSESS") || name.starts_with("B_MM_ASSESS")) return "perceptions";
  if(name.starts_with("DIA_") || name.starts_with("INFO_")) return "dialogs";
  return "other";
  }

void DayScriptStats::call(const daedalus::DaedalusSymbol& symbol, const daedalus::DaedalusVm& vm) {
  ++calls;
  ++symbols[symbol.name()];
  auto kind=family(symbol.name());
  if(symbol.type()==daedalus::DaedalusDataType::INSTANCE || symbol.type()==daedalus::DaedalusDataType::PROTOTYPE) {
    auto* parent=&symbol;
    while(parent && parent->type()!=daedalus::DaedalusDataType::CLASS) parent=vm.find_symbol_by_index(parent->parent());
    if(parent && parent->name()=="C_NPC") kind="npc_creation";
    if(parent && parent->name()=="C_ITEM") kind="item_creation";
    }
  if(kind=="npc_creation" || kind=="item_creation") creationKinds[symbol.name()]=kind;
  if(auto* p=phase()) {
    auto& root=contexts.back().root;
    if(root==RootCall::Pending)
      root=RootCall::Observed;
    else
      ++p->nested;
    }
  stack.push_back({symbol.name(),symbol.address()});
  }

void DayScriptStats::instruction(const daedalus::DaedalusInstruction& instruction, uint32_t pc) {
  // An overridden BL can return without entering bytecode. It still counts as a
  // call, but must not become the top-level owner of later random draws.
  while(!stack.empty() && stack.back().instructions==nullptr && stack.back().address!=pc) stack.pop_back();
  if(stack.empty()) {
    ++unattributedInstructions;
    } else {
    auto& frame=stack.back();
    if(frame.instructions==nullptr)
      frame.instructions=&instructionsBySymbol[frame.name];
    ++*frame.instructions;
    }
  if(auto* p=phase()) ++p->instructions;
  if(instruction.op==daedalus::DaedalusOpcode::RSR && !stack.empty()) stack.pop_back();
  }

void DayScriptStats::external(const daedalus::DaedalusSymbol& symbol) {
  if(auto* p=phase()) ++p->externals[symbol.name()];
  }

DayScriptStats::Phase* DayScriptStats::phase() {
  if(contexts.empty() || contexts.back().state.empty()) return nullptr;
  const auto& c=contexts.back();
  return &states[c.state].phases[c.phase];
  }

void DayScriptStats::begin(std::string_view symbol) {
  auto [state,phase]=statePhase(symbol);
  if(state.empty() && !symbol.starts_with("RTN_")) perceptions.insert(std::string(symbol));
  contexts.push_back({std::move(state),std::move(phase)});
  if(auto* p=this->phase()) ++p->engine;
  }

void DayScriptStats::record() {
  if(contexts.empty() || contexts.back().root==RootCall::Record)
    return;
  auto& root=contexts.back().root;
  if(auto* p=phase()) {
    ++p->records;
    // Some native handlers report acceptance after running their helpers.
    if(root==RootCall::Observed)
      ++p->nested;
    }
  root=RootCall::Record;
  }

void DayScriptStats::end() {
  contexts.pop_back();
  }

std::string DayScriptStats::source(bool script, bool actor) const {
  if(!script) return actor ? "native_combat" : "native_other";
  if(!contexts.empty() && !contexts.back().state.empty()) return "HLP_RANDOM/"+contexts.back().state+"/"+contexts.back().phase;
  return "HLP_RANDOM/outside/"+(stack.empty() ? std::string("<engine>") : stack.front().name);
  }

void DayScriptStats::random(bool script, bool actor) {
  if(!active) return;
  ++rolls;
  ++sources[source(script,actor)];
  if(script) if(auto* p=phase()) ++p->random;
  }

std::string DayScriptStats::json() const {
  std::map<std::string,uint64_t> families;
  for(const auto& [name,count]:symbols) {
    const auto found=creationKinds.find(name);
    families[found==creationKinds.end() ? family(name) : found->second]+=count;
    }
  std::string out="{\"metric\":\"vm_instructions_inclusive\",\"calls\":"+std::to_string(calls)+",\"random\":"+std::to_string(rolls)+
                  ",\"families\":"+counts(families)+",\"symbols\":"+counts(symbols)+
                  ",\"instructions_by_symbol\":"+counts(instructionsBySymbol)+
                  ",\"unattributed_instructions\":"+std::to_string(unattributedInstructions)+
                  ",\"random_sources\":"+counts(sources)+",\"states\":{";
  bool first=true;
  for(const auto& [name,state]:states) {
    if(!first) out+=",";
    first=false;
    out+=jsonEscape(name)+":{\"rows\":"+std::to_string(state.rows)+",\"phases\":{";
    bool firstPhase=true;
    for(const auto& phaseName:{"init","loop","end"}) {
      const auto found=state.phases.find(phaseName);
      const auto p=found==state.phases.end() ? Phase{} : found->second;
      if(!firstPhase) out+=",";
      firstPhase=false;
      out+=jsonEscape(phaseName)+":{\"engine\":"+std::to_string(p.engine)+",\"records\":"+std::to_string(p.records)+
           ",\"nested\":"+std::to_string(p.nested)+
           ",\"instructions\":"+std::to_string(p.instructions)+",\"random\":"+std::to_string(p.random)+",\"externals\":"+counts(p.externals)+"}";
      }
    out+="}}";
    }
  return out+"}}";
  }

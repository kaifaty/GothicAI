#include "baseimport.h"
#include "npcsnapshot.h"
#include "npchelperdefinitions.h"
#include "npcdefinitions.h"
#include "daedalus/classes.h"
#include "daedalus/vm.h"
#include "programdata.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>

namespace fs = std::filesystem;
namespace {
using namespace daedalus;
using namespace NpcHelperDefinitions;
using Type = DaedalusDataType;
using Op = DaedalusOpcode;
using Kind = Value::Kind;
using Field = NpcDefinitions::Field;
using Call = NpcDefinitions::Call;
using Key = std::pair<std::string,uint16_t>;

struct Stop : std::runtime_error {
  std::string reason, detail;
  Stop(std::string reason,std::string detail):std::runtime_error(reason+": "+detail),reason(std::move(reason)),detail(std::move(detail)) {}
  };
void require(bool condition,const std::string& why) {
  if(!condition) throw Stop("unsupported_template",why);
  }
Value number(int32_t value) { Value v; v.integer=value; return v; }
Value self() { Value v; v.kind=Kind::Self; return v; }
Value text(std::string value) { Value v; v.kind=Kind::String; v.text=std::move(value); return v; }
struct Draw {
  int32_t bound = 0, result = 0;
  bool operator==(const Draw&) const = default;
  };
using Event = std::variant<Field,Call,Draw>;
struct Situation {
  std::vector<Value> args;
  std::map<Key,int32_t> fields;
  int32_t roll = 0, equippedStrength = 0;
  };
struct Run {
  std::vector<Event> events;
  std::map<std::string,Value> locals;
  std::vector<std::pair<std::string,std::vector<Value>>> calls;
  std::set<Key> reads;
  std::map<uint32_t,uint8_t> branches;
  };

bool local(const DaedalusSymbol& symbol,const DaedalusVm& vm) {
  const auto dot=symbol.name().find('.');
  const auto* owner=dot==std::string::npos ? nullptr : vm.find_symbol_by_name(symbol.name().substr(0,dot));
  return owner && owner->type()==Type::FUNCTION;
  }
class ScriptRun {
  public:
    ScriptRun(const NpcSnapshot::Program& program):vm(load(program)) {
      require(vm.global_self()!=nullptr,"program has no SELF");
      npc=vm.allocate_instance<INpc>(vm.global_self());
      vm.register_exception_handler([](DaedalusVm&,const DaedalusScriptError& error,const DaedalusInstruction&)
                                      -> DaedalusVmExceptionStrategy { throw Stop("script_error",error.what()); });
      vm.observe_calls([&](const DaedalusSymbol& function) {
        if(function.type()==Type::FUNCTION && !function.is_external()) {
          const auto parameters=vm.find_parameters_for_function(&function);
          std::vector<Value> args(parameters.size());
          for(size_t i=0;i<args.size();++i) {
            const auto& frame=vm.stack_frame(args.size()-i-1);
            args[i]=frameValue(frame,parameters[i].type());
            }
          result.calls.emplace_back(function.name(),std::move(args));
          }
        });
      vm.observe_instructions([&](const DaedalusInstruction& instruction) {
        if(++instructions>100000) throw Stop("execution_limit",root);
        if(instruction.op==Op::BZ)
          result.branches[vm.pc()]|=frameValue(vm.stack_frame(0),Type::INT).integer==0 ? 1 : 2;
        });
      vm.observe_reads([&](const DaedalusSymbol& symbol) {
        if(symbol.is_member() || symbol.is_const() || &symbol==vm.global_self() ||
           (symbol.is_generated() && symbol.type()==Type::STRING)) return;
        if(!local(symbol,vm)) throw Stop("game_state",symbol.name());
        if(!assigned.contains(symbol.index())) throw Stop("local_state",symbol.name()+" before assignment");
        });
      vm.observe_writes([&](const DaedalusSymbol& symbol) {
        if(symbol.is_member()) return;
        if(!local(symbol,vm)) throw Stop("game_state_write",symbol.name());
        assigned.insert(symbol.index());
        });
      vm.observe_member_reads([&](const DaedalusSymbol& symbol,uint16_t index,const DaedalusInstance* instance) {
        if(instance!=npc.get() || !symbol.name().starts_with("C_NPC.")) throw Stop("unsupported_read",symbol.name());
        result.reads.emplace(symbol.name(),index);
        });
      vm.observe_member_writes([&](const DaedalusSymbol& symbol,uint16_t index,const DaedalusInstance* instance) {
        if(instance!=npc.get() || !symbol.name().starts_with("C_NPC.") || symbol.type()!=Type::INT)
          throw Stop("unsupported_write",symbol.name());
        result.events.emplace_back(Field{symbol.name(),index,number(symbol.get_int(index,instance))});
        });
      vm.register_default_external_custom([&](DaedalusVm&,DaedalusSymbol& function) { external(function); });
      }
    Run run(const std::string& helper,const Situation& situation) {
      root=helper; input=&situation;
      for(const auto& [key,value]:situation.fields) {
        auto* binding=vm.find_symbol_by_name(key.first);
        require(binding && binding->is_member() && key.second<binding->count(),"invalid input field "+key.first);
        binding->set_int(value,key.second,npc.get());
        }
      const auto* function=vm.find_symbol_by_name(helper);
      require(function && function->type()==Type::FUNCTION && !function->is_external() && !function->has_return(),helper);
      require(function->count()==situation.args.size(),helper+" signature");
      for(const auto& argument:situation.args) push(argument);
      vm.unsafe_call(function);
      for(auto index:assigned) {
        const auto* symbol=vm.find_symbol_by_index(index);
        result.locals.emplace(symbol->name(),symbolValue(*symbol));
        }
      return std::move(result);
      }
  private:
    static DaedalusVm load(const NpcSnapshot::Program& program) {
      auto script=DaedalusScript::from_program(program.data);
      if(script.find_symbol_by_name("C_NPC")) INpc::register_(script);
      if(script.find_symbol_by_name("C_ITEM")) IItem::register_(script);
      return DaedalusVm(std::move(script));
      }
    Value instanceValue(const std::shared_ptr<DaedalusInstance>& instance) const {
      if(instance==npc) return self();
      Value value; value.kind=Kind::Null;
      require(instance==nullptr,"foreign NPC instance");
      return value;
      }
    Value symbolValue(const DaedalusSymbol& symbol,uint16_t index=0,const DaedalusInstance* context=nullptr) const {
      if(symbol.type()==Type::INSTANCE) return instanceValue(const_cast<DaedalusSymbol&>(symbol).get_instance());
      if(symbol.type()==Type::STRING) return text(symbol.get_string(index,context));
      if(symbol.type()==Type::FLOAT) { Value value; value.kind=Kind::Float; value.real=symbol.get_float(index,context); return value; }
      return number(symbol.get_int(index,context));
      }
    Value frameValue(const DaedalusStackFrame& frame,Type type) const {
      if(frame.reference) return symbolValue(*std::get<DaedalusSymbol*>(frame.value),frame.index,frame.context.get());
      if(type==Type::INSTANCE) return instanceValue(std::get<std::shared_ptr<DaedalusInstance>>(frame.value));
      if(type==Type::FLOAT) { Value v; v.kind=Kind::Float; v.real=std::get<float>(frame.value); return v; }
      return number(std::get<int32_t>(frame.value));
      }
    int32_t integer(const Value& value) const {
      if(value.kind==Kind::Symbol) {
        const auto* symbol=vm.find_symbol_by_name(value.text);
        require(symbol!=nullptr,"unknown symbol "+value.text);
        return int32_t(symbol->index());
        }
      require(value.kind==Kind::Int || value.kind==Kind::Null,"expected integer argument");
      return value.integer;
      }
    void push(const Value& value) {
      if(value.kind==Kind::Self) vm.push_instance(npc);
      else if(value.kind==Kind::String) {
        for(const auto& symbol:vm.symbols())
          if(symbol.is_const() && symbol.type()==Type::STRING && !symbol.is_member())
            for(uint16_t i=0;i<symbol.count();++i)
              if(symbol.get_string(i)==value.text) { vm.push_reference(vm.find_symbol_by_index(symbol.index()),uint8_t(i)); return; }
        vm.push_string(value.text);
        }
      else if(value.kind==Kind::Float) vm.push_float(value.real);
      else vm.push_int(integer(value));
      }
    void external(DaedalusSymbol& function) {
      static const std::set<std::string> known={"MDL_SETVISUAL","MDL_SETVISUALBODY","MDL_SETMODELSCALE",
                                               "MDL_APPLYOVERLAYMDS","CREATEINVITEMS","NPC_SETTALENTSKILL","HLP_RANDOM"};
      require(known.contains(function.name()),"external "+function.name());
      const auto parameters=vm.find_parameters_for_function(&function);
      Call call; call.function=function.name(); call.args.resize(parameters.size());
      for(size_t i=parameters.size();i>0;--i) {
        auto& value=call.args[i-1];
        switch(parameters[i-1].type()) {
          case Type::INSTANCE: value=instanceValue(vm.pop_instance()); break;
          case Type::STRING: value=text(vm.pop_string()); break;
          case Type::FLOAT: value.kind=Kind::Float; value.real=vm.pop_float(); break;
          case Type::INT: value=number(vm.pop_int()); break;
          default: throw Stop("unsupported_parameter",function.name());
          }
        const auto& name=call.function;
        if((name=="CREATEINVITEMS" && i==2) || (name=="MDL_SETVISUALBODY" && i==8)) {
          if(value.integer>0) {
            const auto* symbol=vm.find_symbol_by_index(uint32_t(value.integer));
            require(symbol!=nullptr,"unknown item symbol");
            value.kind=Kind::Symbol; value.text=symbol->name(); value.integer=0;
            }
          }
        }
      if(call.function=="HLP_RANDOM") {
        require(call.args.size()==1 && input->roll>=0 && input->roll<call.args[0].integer,"random range");
        result.events.emplace_back(Draw{call.args[0].integer,input->roll});
        vm.push_int(input->roll);
        }
      else {
        result.events.emplace_back(call);
        if(call.function=="MDL_SETVISUALBODY") npc->attribute[4]+=input->equippedStrength;
        require(!function.has_return(),"external return "+function.name());
        }
      }
    DaedalusVm vm;
    std::shared_ptr<INpc> npc;
    const Situation* input = nullptr;
    Run result;
    std::set<uint32_t> assigned;
    size_t instructions = 0;
    std::string root;
  };

class StubHost final : public Host {
  public:
    explicit StubHost(const Situation& input):input(input),fields(input.fields) {}
    int32_t get(std::string_view binding,uint16_t index) override { return fields[{std::string(binding),index}]; }
    void set(std::string_view binding,uint16_t index,int32_t value) override {
      fields[{std::string(binding),index}]=value;
      result.events.emplace_back(Field{std::string(binding),index,number(value)});
      }
    void external(std::string_view function,std::span<const Value> arguments) override {
      result.events.emplace_back(Call{std::string(function),{arguments.begin(),arguments.end()},false});
      if(function=="MDL_SETVISUALBODY") fields[{"C_NPC.ATTRIBUTE",4}]+=input.equippedStrength;
      }
    int32_t random(int32_t bound) override { result.events.emplace_back(Draw{bound,input.roll}); return input.roll; }
    void local(std::string_view name,const Value& value) override { result.locals[std::string(name)]=value; }
    const Situation& input;
    std::map<Key,int32_t> fields;
    Run result;
  };

Run execute(const NpcSnapshot::Program& program,const std::string& helper,const Situation& input) {
  // Each case starts with pristine function locals and globals, not the preceding probe's VM.
  return ScriptRun(program).run(helper,input);
  }
const Call& call(const Run& result,std::string_view name,size_t ordinal=0) {
  for(const auto& event:result.events)
    if(const auto* value=std::get_if<Call>(&event); value && value->function==name) {
      if(ordinal--==0) return *value;
      }
  throw Stop("unsupported_template","missing "+std::string(name));
  }
std::vector<Field> fields(const Run& result) {
  std::vector<Field> values;
  for(const auto& event:result.events)
    if(const auto* field=std::get_if<Field>(&event)) values.push_back(*field);
  return values;
  }
std::vector<DaedalusInstruction> instructions(const DaedalusScript& script,const std::string& name) {
  const auto* function=script.find_symbol_by_name(name);
  require(function && function->type()==Type::FUNCTION && !function->is_external(),"missing "+name);
  auto end=script.size();
  for(const auto& symbol:script.symbols())
    if(!symbol.is_external() && symbol.address()>function->address() && symbol.address()<end &&
       (symbol.type()==Type::FUNCTION || symbol.type()==Type::PROTOTYPE || symbol.type()==Type::INSTANCE)) end=symbol.address();
  std::vector<DaedalusInstruction> out;
  for(auto pc=function->address();pc<end;) { auto op=script.instruction_at(pc); out.push_back(op); pc+=op.size; }
  return out;
  }
int32_t constant(const DaedalusScript& script,std::string_view name) {
  const auto* symbol=script.find_symbol_by_name(name);
  require(symbol && symbol->is_const() && symbol->type()==Type::INT && symbol->count()==1,"missing constant "+std::string(name));
  return symbol->get_int();
  }

AddFightSkill extractFight(const NpcSnapshot::Program& program,const DaedalusScript& script) {
  AddFightSkill fight;
  const auto code=instructions(script,"B_ADDFIGHTSKILL");
  for(size_t i=3;i+5<code.size();++i) {
    if(code[i].op!=Op::GTE || code[i-3].op!=Op::PUSHI || code[i-2].op!=Op::GMOVI ||
       code[i-1].op!=Op::PUSHVV || code[i+1].op!=Op::BZ || code[i+2].op!=Op::PUSHVI ||
       code[i+3].op!=Op::PUSHV || code[i+4].op!=Op::PUSHI || code[i+5].op!=Op::BE) continue;
    const auto* chance=script.find_symbol_by_index(code[i-1].symbol);
    const auto* talent=script.find_symbol_by_index(code[i+3].symbol);
    const auto* external=script.find_symbol_by_index(code[i+5].symbol);
    require(chance && chance->name()=="C_NPC.HITCHANCE" && talent && talent->is_const() &&
            talent->type()==Type::INT && external && external->name()=="NPC_SETTALENTSKILL","fight threshold shape");
    const auto id=talent->get_int();
    auto found=std::find_if(fight.talents.begin(),fight.talents.end(),[&](const auto& value) { return value.talent==id; });
    if(found==fight.talents.end()) {
      Situation input; input.args={self(),number(id),number(17)};
      const auto written=fields(execute(program,"B_ADDFIGHTSKILL",input));
      require(written.size()==1 && written[0].binding=="C_NPC.HITCHANCE" && written[0].value.integer==17,"fight addition");
      fight.talents.push_back(FightTalent{id,0,int32_t(written[0].index),{}});
      found=std::prev(fight.talents.end());
      }
    found->thresholds.push_back(SkillThreshold{code[i-1].index,code[i-3].immediate,code[i+4].immediate});
    }
  require(!fight.talents.empty(),"no fight talents");
  return fight;
  }
SetFightSkills extractSetFight(const NpcSnapshot::Program& program,const DaedalusScript& script) {
  auto candidates=extractFight(program,script);
  Situation input; input.args={self(),number(17)};
  const auto probe=execute(program,"B_SETFIGHTSKILLS",input);
  std::vector<int32_t> aivars;
  for(const auto& field:fields(probe))
    if(field.binding=="C_NPC.AIVAR") aivars.push_back(field.index);
  SetFightSkills result;
  for(const auto& [name,args]:probe.calls)
    if(name=="B_RAISEFIGHTTALENT") {
      require(args.size()==3,"raise-fight signature");
      const auto found=std::find_if(candidates.talents.begin(),candidates.talents.end(),[&](const auto& value) { return value.talent==args[1].integer; });
      require(found!=candidates.talents.end() && result.fight.talents.size()<aivars.size(),"raise-fight mapping");
      auto talent=*found;
      talent.aivar=aivars[result.fight.talents.size()];
      result.fight.talents.push_back(std::move(talent));
      }
  require(result.fight.talents.size()==candidates.talents.size() && result.fight.talents.size()==aivars.size(),"set-fight order");
  return result;
  }
SetNpcVisual extractVisual(const NpcSnapshot::Program& program,const DaedalusScript& script) {
  SetNpcVisual visual;
  visual.male=constant(script,"MALE");
  const auto code=instructions(script,"B_SETNPCVISUAL");
  bool thin=false,strong=false,first=false,last=false;
  for(size_t i=0;i+3<code.size();++i) {
    if(code[i].op==Op::PUSHI && code[i+1].op==Op::GMOVI && code[i+2].op==Op::PUSHVV &&
       (code[i+3].op==Op::LT || code[i+3].op==Op::GT)) {
      const auto* field=script.find_symbol_by_index(code[i+2].symbol);
      require(field && field->name()=="C_NPC.ATTRIBUTE","visual scale input");
      visual.strength=code[i+2].index;
      if(code[i+3].op==Op::LT) { visual.thinLimit=code[i].immediate; thin=true; }
      else { visual.strongLimit=code[i].immediate; strong=true; }
      }
    if(code[i].op==Op::PUSHI && code[i+1].op==Op::PUSHV &&
       (code[i+2].op==Op::LTE || code[i+2].op==Op::GTE)) {
      const auto* parameter=script.find_symbol_by_index(code[i+1].symbol);
      if(parameter && parameter->name()=="B_SETNPCVISUAL.BODYTEX") {
        if(code[i+2].op==Op::GTE) { visual.femaleTextureFirst=code[i].immediate; first=true; }
        else { visual.femaleTextureLast=code[i].immediate; last=true; }
        }
      }
    }
  require(thin && strong && first && last,"visual branch limits");
  Situation input; input.args={self(),number(visual.male),text("HELPER_HEAD"),number(23),number(visual.femaleTextureFirst),number(-1)};
  input.fields[{"C_NPC.ATTRIBUTE",uint16_t(visual.strength)}]=visual.thinLimit-1;
  const auto light=execute(program,"B_SETNPCVISUAL",input);
  const auto written=fields(light);
  require(written.size()==1 && written[0].binding=="C_NPC.AIVAR","visual gender field");
  visual.genderAivar=written[0].index;
  visual.model=call(light,"MDL_SETVISUAL").args.at(1).text;
  const auto body=call(light,"MDL_SETVISUALBODY");
  require(body.args.size()==8,"body signature");
  visual.maleBody=body.args[1].text; visual.bodyColor=body.args[3].integer; visual.teeth=body.args[6].integer;
  const auto thinScale=call(light,"MDL_SETMODELSCALE");
  for(size_t i=0;i<3;++i) visual.thinScale[i]=thinScale.args.at(i+1).real;
  input.fields[{"C_NPC.ATTRIBUTE",uint16_t(visual.strength)}]=visual.strongLimit+1;
  const auto heavy=execute(program,"B_SETNPCVISUAL",input);
  const auto strongScale=call(heavy,"MDL_SETMODELSCALE");
  for(size_t i=0;i<3;++i) visual.strongScale[i]=strongScale.args.at(i+1).real;
  input.args[1]=number(visual.male+1);
  const auto female=execute(program,"B_SETNPCVISUAL",input);
  const auto femaleBody=call(female,"MDL_SETVISUALBODY");
  visual.femaleBody=femaleBody.args[1].text;
  visual.femaleTextureOffset=femaleBody.args[2].integer-visual.femaleTextureFirst;
  return visual;
  }
InventoryTable inventory(const Run& run,int32_t bound) {
  InventoryTable table;
  for(const auto& [name,args]:run.calls)
    if(name.starts_with("B_CREATEAMBIENTINV_")) { require(table.helper.empty(),"nested ambient table"); table.helper=name; }
  require(!table.helper.empty(),"ambient table helper");
  table.rolls.resize(size_t(bound));
  return table;
  }
std::vector<InventoryItem> items(const Run& run) {
  std::vector<InventoryItem> result;
  for(const auto& event:run.events) {
    if(std::holds_alternative<Draw>(event)) continue;
    const auto* value=std::get_if<Call>(&event);
    require(value && value->function=="CREATEINVITEMS" && value->args.size()==3 &&
            value->args[0].kind==Kind::Self && value->args[1].kind==Kind::Symbol,"ambient item call");
    result.push_back({value->args[1].text,value->args[2].integer});
    }
  return result;
  }
AmbientInventory extractAmbient(const NpcSnapshot::Program& program,const DaedalusScript& script) {
  Situation input; input.args={self()}; input.fields[{"C_NPC.GUILD",0}]=-1;
  const auto probe=execute(program,"B_CREATEAMBIENTINV",input);
  require(!probe.events.empty() && std::holds_alternative<Draw>(probe.events[0]),"ambient first random draw");
  AmbientInventory ambient;
  ambient.randomBound=std::get<Draw>(probe.events[0]).bound;
  require(ambient.randomBound>0 && ambient.randomBound<=1024,"ambient random limit");
  const auto tableFor=[&](int32_t guild) {
    input.fields[{"C_NPC.GUILD",0}]=guild; input.roll=0;
    const auto first=execute(program,"B_CREATEAMBIENTINV",input);
    auto table=inventory(first,ambient.randomBound);
    table.rolls[0]=items(first);
    for(int32_t roll=1;roll<ambient.randomBound;++roll) {
      input.roll=roll;
      const auto result=execute(program,"B_CREATEAMBIENTINV",input);
      require(inventory(result,ambient.randomBound).helper==table.helper,"ambient selector depends on roll");
      table.rolls[size_t(roll)]=items(result);
      }
    return table;
    };
  ambient.fallback=tableFor(-1);
  std::set<int32_t> guilds;
  const auto code=instructions(script,"B_CREATEAMBIENTINV");
  for(size_t i=0;i+3<code.size();++i)
    if(code[i].op==Op::PUSHV && code[i+1].op==Op::GMOVI &&
       (code[i+2].op==Op::PUSHV || (code[i+2].op==Op::PUSHVV && code[i+2].index==0)) && code[i+3].op==Op::EQ) {
      const auto* guild=script.find_symbol_by_index(code[i].symbol);
      const auto* field=script.find_symbol_by_index(code[i+2].symbol);
      if(field && field->name()=="C_NPC.GUILD") {
        require(guild && guild->is_const() && guild->type()==Type::INT,"ambient guild selector");
        guilds.insert(guild->get_int());
        }
      }
  for(auto guild:guilds) ambient.guilds.emplace(guild,tableFor(guild));
  require(!ambient.guilds.empty(),"no guild constants");
  return ambient;
  }
SkeletonLord extractSkeleton(const NpcSnapshot::Program& program,const DaedalusScript& script) {
  SkeletonLord skeleton;
  skeleton.visual=extractVisual(program,script);
  const auto probe=execute(program,"B_SETVISUALS_SKELETON_LORD",Situation{});
  skeleton.model=call(probe,"MDL_SETVISUAL").args.at(1).text;
  for(const auto& event:probe.events)
    if(const auto* value=std::get_if<Call>(&event); value && value->function=="MDL_APPLYOVERLAYMDS") skeleton.overlays.push_back(value->args.at(1).text);
  const auto nested=std::find_if(probe.calls.begin(),probe.calls.end(),[](const auto& value) { return value.first=="B_SETNPCVISUAL"; });
  require(nested!=probe.calls.end() && nested->second.size()==6,"skeleton visual helper");
  const auto& args=nested->second;
  skeleton.gender=args[1].integer; skeleton.head=args[2].text; skeleton.faceTexture=args[3].integer; skeleton.bodyTexture=args[4].integer;
  const auto* armor=script.find_symbol_by_index(uint32_t(args[5].integer));
  require(armor && armor->type()==Type::INSTANCE,"skeleton armor");
  skeleton.armor=armor->name();
  return skeleton;
  }
AttributesToChapter extractAttributes(const NpcSnapshot::Program& program,const DaedalusScript& script) {
  AttributesToChapter attributes;
  const auto code=instructions(script,"B_SETATTRIBUTESTOCHAPTER");
  for(size_t i=0;i+3<code.size();++i) {
    if(code[i].op!=Op::PUSHI || code[i+1].op!=Op::PUSHV ||
       (code[i+2].op!=Op::EQ && code[i+2].op!=Op::GTE) || code[i+3].op!=Op::BZ) continue;
    const auto* parameter=script.find_symbol_by_index(code[i+1].symbol);
    if(parameter && parameter->name()=="B_SETATTRIBUTESTOCHAPTER.KAP") {
      ChapterAttributes chapter;
      chapter.chapter=code[i].immediate; chapter.minimum=code[i+2].op==Op::GTE;
      Situation input; input.args={self(),number(chapter.chapter)};
      const auto result=execute(program,"B_SETATTRIBUTESTOCHAPTER",input);
      if(attributes.chapters.empty()) {
        const auto& talent=call(result,"NPC_SETTALENTSKILL");
        attributes.talent=talent.args.at(1).integer; attributes.skill=talent.args.at(2).integer;
        }
      chapter.fields=fields(result);
      require(chapter.fields.size()>=2,"chapter attribute writes");
      require(chapter.fields[chapter.fields.size()-2].binding=="C_NPC.EXP" && chapter.fields.back().binding=="C_NPC.EXP_NEXT","chapter experience writes");
      chapter.fields.resize(chapter.fields.size()-2);
      attributes.chapters.push_back(std::move(chapter));
      }
    }
  require(!attributes.chapters.empty(),"no chapter branches");
  for(size_t i=5;i+1<code.size();++i)
    if(code[i].op==Op::DIV && code[i-5].op==Op::PUSHI && code[i+1].op==Op::PUSHI) {
      const auto divisor=code[i-5].immediate,multiplier=code[i+1].immediate;
      if(attributes.experienceDivisor) require(attributes.experienceDivisor==divisor && attributes.experienceMultiplier==multiplier,"experience formula differs");
      attributes.experienceDivisor=divisor; attributes.experienceMultiplier=multiplier;
      }
  require(attributes.experienceDivisor>0,"experience divisor");
  return attributes;
  }

std::map<std::string,std::vector<Situation>> corpus(const fs::path& npcs,std::string_view sha) {
  const auto library=NpcDefinitions::read(npcs.parent_path().parent_path().parent_path(),sha);
  std::map<std::string,std::vector<Situation>> helpers;
  for(const auto& [name,definition]:library.npcs) {
    (void)name;
    Situation input;
    for(const auto& step:definition.steps)
      if(const auto* field=std::get_if<Field>(&step)) {
        if(field->value.kind==Kind::Int) input.fields[{field->binding,field->index}]=field->value.integer;
        }
      else if(const auto* call=std::get_if<Call>(&step); call && call->deferred) {
        input.args=call->args;
        helpers[call->function].push_back(input);
        }
    }
  return helpers;
  }
std::set<int32_t> boundaries(int32_t value) {
  std::set<int32_t> result{value};
  if(value>INT32_MIN) result.insert(value-1);
  if(value<INT32_MAX) result.insert(value+1);
  return result;
  }
std::vector<Situation> cases(const Definition& definition,const std::vector<Situation>& real,const DaedalusScript& script) {
  std::vector<Situation> result=real;
  auto add=[&](Situation input) { result.push_back(std::move(input)); };
  std::visit([&](const auto& helper) {
    using T=std::decay_t<decltype(helper)>;
    if constexpr(std::is_same_v<T,SetFightSkills> || std::is_same_v<T,AddFightSkill>) {
      const AddFightSkill& fight=[&]() -> const AddFightSkill& {
        if constexpr(std::is_same_v<T,SetFightSkills>) return helper.fight; else return helper;
        }();
      std::set<int32_t> chances{-1,0,1},percents{-1,0,1,13};
      for(const auto& talent:fight.talents)
        for(const auto& stage:talent.thresholds) {
          const auto values=boundaries(stage.threshold); chances.insert(values.begin(),values.end());
          percents.insert(values.begin(),values.end());
          }
      for(auto percent:percents) {
        Situation input;
        if constexpr(std::is_same_v<T,SetFightSkills>) input.args={self(),number(percent)};
        else input.args={self(),number(-1),number(percent)};
        add(input);
        for(const auto& talent:fight.talents) {
          if constexpr(std::is_same_v<T,AddFightSkill>) input.args[1]=number(talent.talent);
          for(auto target:chances) {
            input.fields[{"C_NPC.HITCHANCE",uint16_t(talent.chance)}]=target-percent;
            input.fields[{"C_NPC.AIVAR",uint16_t(talent.aivar)}]=73;
            add(input);
            // Vary other chances independently: the original 2H stage zero reads 1H.
            for(const auto& stage:talent.thresholds)
              if(stage.chance!=talent.chance) for(auto other:boundaries(stage.threshold)) {
                auto probe=input;
                probe.fields[{"C_NPC.HITCHANCE",uint16_t(stage.chance)}]=other;
                add(std::move(probe));
                }
            }
          }
        }
      }
    else if constexpr(std::is_same_v<T,SetNpcVisual> || std::is_same_v<T,SkeletonLord>) {
      const SetNpcVisual& visual=[&]() -> const SetNpcVisual& {
        if constexpr(std::is_same_v<T,SetNpcVisual>) return helper; else return helper.visual;
        }();
      auto strengths=boundaries(visual.thinLimit);
      const auto strong=boundaries(visual.strongLimit); strengths.insert(strong.begin(),strong.end());
      for(auto strength:strengths)
        for(auto equipped:{0,visual.strongLimit-visual.thinLimit+2,-(visual.strongLimit-visual.thinLimit+2)}) {
          Situation input;
          if constexpr(std::is_same_v<T,SetNpcVisual>)
            input.args={self(),number(visual.male),text("HELPER_HEAD"),number(23),number(0),number(-1)};
          input.fields[{"C_NPC.ATTRIBUTE",uint16_t(visual.strength)}]=strength;
          input.equippedStrength=equipped;
          add(input);
          }
      if constexpr(std::is_same_v<T,SetNpcVisual>) {
        auto textures=boundaries(visual.femaleTextureFirst);
        const auto last=boundaries(visual.femaleTextureLast); textures.insert(last.begin(),last.end());
        for(auto gender:{visual.male-1,visual.male,visual.male+1})
          for(auto body:textures) {
            Situation input; input.args={self(),number(gender),text("HELPER_HEAD"),number(-1),number(body),number(-1)};
            add(input);
            }
        }
      }
    else if constexpr(std::is_same_v<T,AmbientInventory>) {
      std::set<int32_t> guilds{-1};
      for(const auto& [guild,table]:helper.guilds) { (void)table; guilds.insert(guild); }
      for(const auto& symbol:script.symbols())
        if(symbol.is_const() && symbol.type()==Type::INT && symbol.count()==1 && symbol.name().starts_with("GIL_")) guilds.insert(symbol.get_int());
      for(const auto& input:real) {
        const auto value=input.fields.find({"C_NPC.GUILD",0});
        guilds.insert(value==input.fields.end() ? 0 : value->second);
        }
      for(auto guild:guilds)
        for(int32_t roll=0;roll<helper.randomBound;++roll) {
          Situation input; input.args={self()}; input.fields[{"C_NPC.GUILD",0}]=guild; input.roll=roll; add(input);
          }
      }
    else if constexpr(std::is_same_v<T,AttributesToChapter>) {
      std::set<int32_t> chapters{-1};
      for(const auto& chapter:helper.chapters) {
        const auto values=boundaries(chapter.chapter); chapters.insert(values.begin(),values.end());
        }
      for(auto chapter:chapters)
        for(auto level:{-3,-1,0,1,2,3,73,10000}) {
          Situation input; input.args={self(),number(chapter)}; input.fields[{"C_NPC.LEVEL",0}]=level; add(input);
          }
      }
    },definition);
  return result;
  }

Value normalize(Value value,const DaedalusScript& script) {
  if(value.kind==Kind::Symbol) {
    const auto* symbol=script.find_symbol_by_name(value.text);
    require(symbol!=nullptr,"unknown local symbol "+value.text);
    value=number(int32_t(symbol->index()));
    }
  return value;
  }
Event normalize(Event event,const DaedalusScript& script) {
  if(auto* call=std::get_if<Call>(&event))
    for(auto& argument:call->args) argument=normalize(std::move(argument),script);
  return event;
  }
std::string eventText(const Event& event) {
  if(const auto* field=std::get_if<Field>(&event)) return field->binding+"["+std::to_string(field->index)+"]="+std::to_string(field->value.integer);
  if(const auto* call=std::get_if<Call>(&event)) {
    std::string out=call->function+"(";
    for(size_t i=0;i<call->args.size();++i) {
      if(i) out+=",";
      const auto& argument=call->args[i];
      switch(argument.kind) {
        case Kind::Self: out+="self"; break;
        case Kind::Null: out+="null"; break;
        case Kind::Int: out+=std::to_string(argument.integer); break;
        case Kind::Float: out+="float_bits:"+std::to_string(std::bit_cast<uint32_t>(argument.real)); break;
        case Kind::String: out+='"'+argument.text+'"'; break;
        case Kind::Symbol: out+=argument.text; break;
        }
      }
    return out+")";
    }
  const auto& draw=std::get<Draw>(event);
  return "HLP_RANDOM("+std::to_string(draw.bound)+")->"+std::to_string(draw.result);
  }
size_t compare(const NpcSnapshot::Program& program,const DaedalusScript& script,const std::string& helper,
               const Definition& definition,const std::vector<Situation>& inputs) {
  const auto situations=cases(definition,inputs,script);
  std::map<uint32_t,uint8_t> branches;
  for(size_t index=0;index<situations.size();++index) {
    const auto& input=situations[index];
    const auto expected=execute(program,helper,input);
    for(const auto& [pc,mask]:expected.branches) branches[pc]|=mask;
    StubHost host(input);
    NpcHelperDefinitions::run(definition,input.args,host);
    const auto& actual=host.result;
    size_t event=0;
    while(event<expected.events.size() && event<actual.events.size() &&
          normalize(expected.events[event],script)==normalize(actual.events[event],script)) ++event;
    const auto where=helper+" case "+std::to_string(index+1)+": ";
    if(event<expected.events.size() || event<actual.events.size())
      throw Stop("template_mismatch",where+"step "+std::to_string(event+1)+" VM "+
                 (event<expected.events.size() ? eventText(expected.events[event]) : "<none>")+", template "+
                 (event<actual.events.size() ? eventText(actual.events[event]) : "<none>"));
    std::map<std::string,Value> locals;
    for(const auto& [name,value]:actual.locals) locals.emplace(name,normalize(value,script));
    if(expected.locals!=locals) {
      for(const auto& [name,value]:expected.locals)
        if(!locals.contains(name) || locals.at(name)!=value) throw Stop("local_mismatch",where+name);
      throw Stop("local_mismatch",where+"unexpected local");
      }
    }
  auto pc=script.find_symbol_by_name(helper)->address();
  for(const auto& instruction:instructions(script,helper)) {
    if(instruction.op==Op::BZ && branches[pc]!=3)
      throw Stop("branch_coverage",helper+" at "+std::to_string(pc)+": comparison inputs do not cover both outcomes");
    pc+=instruction.size;
    }
  return situations.size();
  }
struct Capture {
  Library library;
  NpcSnapshot::HelperCounts counts;
  };
Capture recognize(const NpcSnapshot::Program& program,const fs::path& npcs,bool announce) {
  Capture result;
  const auto script=DaedalusScript::from_program(program.data);
  for(const auto& [name,inputs]:corpus(npcs,program.sha256)) {
    ++result.counts.total; result.counts.deferred+=inputs.size();
    try {
      Definition definition;
      if(name=="B_SETFIGHTSKILLS") definition=extractSetFight(program,script);
      else if(name=="B_SETNPCVISUAL") definition=extractVisual(program,script);
      else if(name=="B_CREATEAMBIENTINV") definition=extractAmbient(program,script);
      else if(name=="B_ADDFIGHTSKILL") definition=extractFight(program,script);
      else if(name=="B_SETVISUALS_SKELETON_LORD") definition=extractSkeleton(program,script);
      else if(name=="B_SETATTRIBUTESTOCHAPTER") definition=extractAttributes(program,script);
      else throw Stop("unknown_helper",name);
      const auto situations=compare(program,script,name,definition,inputs);
      result.counts.situations+=situations;
      result.library.helpers.emplace(name,std::move(definition));
      ++result.counts.captured;
      if(announce) std::cout<<"NPC helper "<<name<<": captured ("<<inputs.size()<<" deferred calls, "<<situations<<" compared situations)\n";
      }
    catch(const Stop& stop) {
      result.library.uncaptured.emplace(name,stop.reason+": "+stop.detail);
      if(announce) std::cout<<"NPC helper "<<name<<": remains VM: "<<stop.what()<<'\n';
      }
    }
  return result;
  }
}

NpcSnapshot::HelperCounts NpcSnapshot::writeHelpers(const Program& program,const fs::path& file,const fs::path& npcs) {
  auto result=recognize(program,npcs,true);
  NpcHelperDefinitions::write(result.library,file,program.name,program.sha256);
  return result.counts;
  }
int NpcSnapshot::verifyHelpers(const Program& program,const fs::path& file,const fs::path& npcs) {
  // Disk verification has exactly the game's parser, including duplicate keys and integer types.
  const auto actual=NpcHelperDefinitions::readFile(file,program.sha256);
  const auto expected=recognize(program,npcs,false).library;
  if(actual==expected) return 0;
  int errors=0;
  for(const auto& [name,definition]:expected.helpers)
    if(!actual.helpers.contains(name) || actual.helpers.at(name)!=definition) {
      ++errors; std::cerr<<"NPC helper "<<name<<": parameters differ from fresh VM\n";
      }
  for(const auto& [name,definition]:actual.helpers) {
    (void)definition;
    if(!expected.helpers.contains(name)) { ++errors; std::cerr<<"NPC helper "<<name<<": unexpected captured helper\n"; }
    }
  if(actual.uncaptured!=expected.uncaptured) { ++errors; std::cerr<<"NPC helper VM fallback reasons differ\n"; }
  return errors;
  }
int BaseImport::convertNpcHelperDefinitions(const fs::path& base) {
  const auto start=std::chrono::steady_clock::now();
  const auto index=ProgramData::readIndex(base/"programs");
  size_t total=0,captured=0,programs=0,situations=0,deferred=0;
  for(const auto& [name,sha]:index.programs) {
    const auto npcs=base/"definitions"/sha/"npcs.json";
    if(!fs::exists(npcs)) continue;
    NpcSnapshot::Program program;
    program.name=name;
    program.data=ProgramData::readProgram(index.dir/(name+".json"),&program.sha256);
    require(program.sha256==sha,"helper program SHA differs: "+name);
    ++programs;
    const auto file=base/"definitions"/sha/"helpers.json";
    const auto counts=NpcSnapshot::writeHelpers(program,file,npcs);
    total+=counts.total; captured+=counts.captured; situations+=counts.situations; deferred+=counts.deferred;
    if(NpcSnapshot::verifyHelpers(program,file,npcs)!=0) throw std::runtime_error("NPC helpers: fresh VM comparison failed: "+name);
    }
  const auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();
  std::cout<<"written and verified "<<programs<<" NPC helper programs; "<<total<<" helpers, "<<captured<<" captured, "
           <<(total-captured)<<" remain VM, "<<deferred<<" deferred calls, "<<situations<<" compared situations; 0 failed ("<<ms<<" ms)\n";
  return 0;
  }


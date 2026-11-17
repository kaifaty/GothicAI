#include "baseimport.h"
#include "npcsnapshot.h"
#include "asset/npcdefinitions.h"
#include "daedalus/classes.h"
#include "daedalus/vm.h"
#include "npcsnapshotfields.h"
#include "programdata.h"

#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>

namespace fs = std::filesystem;
namespace {
using namespace daedalus;
using namespace NpcDefinitions;
using Type = DaedalusDataType;
using Op = DaedalusOpcode;
using Json = rapidjson::Value;
using Allocator = rapidjson::Document::AllocatorType;

void require(bool ok,const std::string& why) {
  if(!ok) throw std::runtime_error(why);
  }
struct Stop : std::runtime_error {
  std::string reason,detail;
  Stop(std::string reason,std::string detail):std::runtime_error(reason+": "+detail),reason(std::move(reason)),detail(std::move(detail)) {}
  };
struct Defer {
  uint32_t address;
  };
const std::set<std::string> engineCalls={
  "MDL_SETVISUAL","MDL_SETVISUALBODY","MDL_SETMODELFATNESS","MDL_SETMODELSCALE","MDL_APPLYOVERLAYMDS",
  "EQUIPITEM","CREATEINVITEM","CREATEINVITEMS","NPC_SETTALENTSKILL","NPC_SETTALENTVALUE",
  "NPC_SETTOFIGHTMODE","NPC_SETTOFISTMODE","NPC_SETATTITUDE","NPC_SETTEMPATTITUDE","TA_MIN"};
bool mutableGlobal(const DaedalusSymbol& symbol,const DaedalusVm& vm) {
  const auto* parent=vm.find_symbol_by_index(symbol.parent());
  const auto dot=symbol.name().find('.');
  const auto* owner=dot==std::string::npos ? nullptr : vm.find_symbol_by_name(symbol.name().substr(0,dot));
  const bool local=(parent && parent->type()==Type::FUNCTION) || (owner && owner->type()==Type::FUNCTION);
  return !(symbol.is_const() || symbol.is_member() || local ||
           (symbol.is_generated() && symbol.type()==Type::STRING) || symbol.name()=="SELF");
  }
std::string symbolName(const DaedalusScript& script,int32_t index) {
  const auto* symbol=script.find_symbol_by_index(uint32_t(index));
  require(symbol!=nullptr,"invalid symbol reference "+std::to_string(index));
  return symbol->name();
  }
Value reference(const DaedalusScript& script,int32_t index) {
  Value value;
  if(index==0) value.kind=Value::Kind::Null;
  else if(index<0) value.integer=index;
  else { value.kind=Value::Kind::Symbol; value.text=symbolName(script,index); }
  return value;
  }

struct Analysis {
  std::set<uint32_t> random;
  std::map<uint32_t,uint32_t> functions;
  explicit Analysis(const DaedalusScript& script) {
    for(const auto& symbol:script.symbols())
      if(!symbol.is_external() && (symbol.type()==Type::FUNCTION || symbol.type()==Type::PROTOTYPE ||
          (symbol.type()==Type::INSTANCE && symbol.is_const() && symbol.address()>0 && symbol.name().find('.')==std::string::npos)) && symbol.address()<script.size())
        functions.emplace(symbol.address(),symbol.index());
    std::map<uint32_t,std::set<uint32_t>> calls;
    for(auto it=functions.begin();it!=functions.end();++it) {
      const auto end=std::next(it)==functions.end() ? script.size() : std::next(it)->first;
      for(auto pc=it->first;pc<end;) {
        const auto instruction=script.instruction_at(pc);
        if(instruction.op==Op::BL) calls[it->first].insert(instruction.address);
        if(instruction.op==Op::BE) {
          const auto* external=script.find_symbol_by_index(instruction.symbol);
          if(external && external->name()=="HLP_RANDOM") random.insert(it->first);
          }
        pc+=instruction.size;
        }
      }
    bool changed=true;
    while(changed) {
      changed=false;
      for(const auto& [function,edges]:calls)
        for(auto target:edges)
          if(random.count(target) && random.insert(function).second) changed=true;
      }
    }
  };
struct Capture {
  Definition definition;
  std::string reason,detail;
  };

class Runner {
  public:
    explicit Runner(const NpcSnapshot::Program& program):program(program),analysis(DaedalusScript::from_program(program.data)) {}
    Capture run(uint32_t id) const {
      try {
        std::set<uint32_t> deferred;
        for(;;) {
          try { return capture(id,deferred); }
          catch(const Defer& function) {
            require(deferred.insert(function.address).second,"NPC dependency replay did not advance");
            }
          catch(const Stop& error) {
            Capture result;
            result.reason=error.reason; result.detail=error.detail;
            return result;
            }
          }
        }
      catch(const std::exception& error) {
        throw std::runtime_error("NPC definitions: "+program.data.symbols.at(id).name+": "+error.what());
        }
      }
  private:
    Capture capture(uint32_t id,const std::set<uint32_t>& deferred) const {
      Capture result;
      auto script=DaedalusScript::from_program(program.data);
      INpc::register_(script);
      if(script.find_symbol_by_name("C_ITEM")) IItem::register_(script);
      DaedalusVm vm(std::move(script));
      vm.register_exception_handler([](DaedalusVm&,const DaedalusScriptError& error,const DaedalusInstruction&)
                                      -> DaedalusVmExceptionStrategy {
        // The game's lenient handler continues after script errors. Such a
        // constructor needs the original VM path, rather than a partial record.
        throw Stop("script_error",error.what());
        });
      auto npc=std::make_shared<INpc>();
      auto* root=vm.find_symbol_by_index(id);
      require(root!=nullptr,"missing NPC symbol "+std::to_string(id));
      bool engineState=false;
      std::vector<const DaedalusSymbol*> frames;
      auto overridden=[&](const DaedalusSymbol& function) {
        return function.type()==Type::FUNCTION &&
            (analysis.random.count(function.address()) || deferred.count(function.address()));
        };
      vm.observe_calls([&](const DaedalusSymbol& function) {
        if(!overridden(function)) frames.push_back(&function);
        });
      auto validateDeferred=[&](const DaedalusSymbol& function) {
        std::set<uint32_t> visited;
        std::vector<uint32_t> todo{function.address()};
        while(!todo.empty()) {
          const auto address=todo.back(); todo.pop_back();
          if(!visited.insert(address).second) continue;
          require(analysis.functions.contains(address),"missing script function at address "+std::to_string(address));
          const auto next=analysis.functions.upper_bound(address);
          const auto end=next==analysis.functions.end() ? vm.size() : next->first;
          for(auto pc=address;pc<end;) {
            const auto op=vm.instruction_at(pc);
            if(op.op==Op::BL) todo.push_back(op.address);
            if(op.op==Op::BE) {
              const auto* external=vm.find_symbol_by_index(op.symbol);
              require(external!=nullptr,"missing external symbol "+std::to_string(op.symbol));
              if(!engineCalls.count(external->name()) && external->name()!="HLP_RANDOM")
                throw Stop("unsupported_external",external->name());
              if(external->has_return() && external->name()!="HLP_RANDOM")
                throw Stop("external_query",external->name());
              }
            if(op.op==Op::PUSHV || op.op==Op::PUSHVI || op.op==Op::PUSHVV || op.op==Op::GMOVI) {
              const auto* symbol=vm.find_symbol_by_index(op.symbol);
              require(symbol!=nullptr,"missing symbol "+std::to_string(op.symbol));
              if(mutableGlobal(*symbol,vm)) throw Stop("game_state",symbol->name());
              if(symbol==vm.global_self() && pc+op.size<end && vm.instruction_at(pc+op.size).op==Op::MOVVI)
                throw Stop("game_state_write",symbol->name());
              }
            pc+=op.size;
            }
          }
        };
      std::set<std::string> functionFields;
      snapshotFields(*npc,[&](const char*,auto&,bool function,const char* binding) {
        if(function) functionFields.insert(binding);
        });
      // Fields in a call-free interval commute. Keep each assigned element's last value;
      // every engine/script boundary flushes the interval before recording that call.
      std::map<std::pair<std::string,uint16_t>,Value> pending;
      auto flush=[&] {
        for(auto& [key,value]:pending)
          result.definition.steps.emplace_back(Field{key.first,key.second,std::move(value)});
        pending.clear();
        };
      vm.observe_reads([&](const DaedalusSymbol& symbol) {
        if(mutableGlobal(symbol,vm)) throw Stop("game_state",symbol.name());
        });
      vm.observe_member_reads([&](const DaedalusSymbol& symbol,uint16_t index,const DaedalusInstance* context) {
        if(!engineState) return;
        const auto detail=symbol.name()+"["+std::to_string(index)+"] after engine call";
        if(context!=npc.get()) throw Stop("engine_state_read",detail+": unsupported NPC context");
        const auto helper=std::find_if(frames.begin(),frames.end(),[](const auto* frame) {
          return frame->type()==Type::FUNCTION;
          });
        if(helper==frames.end() || (*helper)->has_return()) throw Stop("engine_state_read",detail);
        // Restart the whole constructor in a fresh VM. No pending fields, calls or
        // local/global effects from the partially executed helper survive a retry.
        validateDeferred(**helper);
        throw Defer{(*helper)->address()};
        });
      vm.observe_writes([&](const DaedalusSymbol& symbol) {
        if(mutableGlobal(symbol,vm) || &symbol==vm.global_self())
          throw Stop("game_state_write",symbol.name());
        });
      vm.observe_member_writes([&](const DaedalusSymbol& symbol,uint16_t index,const DaedalusInstance* context) {
        if(context!=npc.get() || !symbol.name().starts_with("C_NPC."))
          throw Stop("unsupported_target",symbol.name());
        Value value;
        switch(symbol.type()) {
          case Type::STRING: value.kind=Value::Kind::String; value.text=symbol.get_string(index,context); break;
          case Type::FLOAT: value.kind=Value::Kind::Float; value.real=symbol.get_float(index,context); break;
          case Type::FUNCTION:
          case Type::INT:
            if(functionFields.count(symbol.name())) value=reference(vm,symbol.get_int(index,context));
            else value.integer=symbol.get_int(index,context);
            break;
          default: throw Stop("unsupported_field",symbol.name());
          }
        pending[{symbol.name(),index}]=std::move(value);
        });
      size_t instructions=0;
      vm.observe_instructions([&](const DaedalusInstruction& op) {
        if(++instructions>1000000) throw Stop("execution_limit",root->name());
        if(op.op==Op::RSR) {
          require(!frames.empty(),"NPC capture call stack is empty");
          frames.pop_back();
          }
        });
      auto arguments=[&](const DaedalusSymbol& function) {
        std::vector<Value> values;
        for(uint32_t i=function.count();i>0;--i) {
          auto* parameter=vm.find_symbol_by_index(function.index()+i);
          require(parameter!=nullptr,"missing parameter of "+function.name());
          Value value;
          switch(parameter->type()) {
            case Type::INSTANCE: {
              if(vm.top_is_reference()) {
                auto [symbol,index,context]=vm.pop_reference();
                (void)index; (void)context;
                require(symbol!=nullptr,"missing instance argument symbol for "+function.name());
                if(mutableGlobal(*symbol,vm)) throw Stop("game_state",symbol->name());
                const auto instance=symbol->get_instance();
                if(instance==npc || symbol==vm.global_self()) value.kind=Value::Kind::Self;
                else { value.kind=Value::Kind::Symbol; value.text=symbol->name(); }
                }
              else {
                const auto instance=vm.pop_instance();
                if(instance==npc) value.kind=Value::Kind::Self;
                else if(instance) { value.kind=Value::Kind::Symbol; value.text=symbolName(vm,int32_t(instance->symbol_index())); }
                else value.kind=Value::Kind::Null;
                }
              break;
              }
            case Type::STRING: value.kind=Value::Kind::String; value.text=vm.pop_string(); break;
            case Type::FLOAT: value.kind=Value::Kind::Float; value.real=vm.pop_float(); break;
            case Type::FUNCTION:
            case Type::INT: {
              const auto integer=vm.pop_int();
              const auto& name=function.name();
              const bool symbol=parameter->type()==Type::FUNCTION ||
                  ((name=="EQUIPITEM" || name=="CREATEINVITEM" || name=="CREATEINVITEMS") && i==2) ||
                  (name=="TA_MIN" && i==6) || (name=="MDL_SETVISUALBODY" && i==8) ||
                  (name=="NPC_SETTOFIGHTMODE" && i==2);
              if(symbol) value=reference(vm,integer);
              else value.integer=integer;
              break;
              }
            default: throw Stop("unsupported_parameter",function.name()+"."+parameter->name());
            }
          values.push_back(std::move(value));
          }
        std::reverse(values.begin(),values.end());
        return values;
        };
      auto findSymbol=[&](const std::string& name) {
        auto* symbol=vm.find_symbol_by_name(name);
        require(symbol!=nullptr,"missing symbol "+name);
        return symbol;
        };
      auto boundNpc=[&](const Value& target) {
        return target.kind==Value::Kind::Self ||
            (target.kind==Value::Kind::Symbol &&
             std::dynamic_pointer_cast<INpc>(findSymbol(target.text)->get_instance())!=nullptr);
        };
      auto integer=[&](const Value& value) {
        return value.kind==Value::Kind::Symbol ? int32_t(findSymbol(value.text)->index()) : value.integer;
        };
      auto validateItemAllocation=[&](const Call& call) {
        if(!boundNpc(call.args.at(0))) return;
        size_t argument=1;
        if(call.function=="CREATEINVITEMS") {
          if(integer(call.args.at(2))<=0) return;
          }
        else if(call.function=="MDL_SETVISUALBODY") {
          argument=7;
          if(integer(call.args.at(argument))<=0) return;
          }
        else if(call.function=="NPC_SETTOFIGHTMODE") {
          if(integer(call.args.at(argument))<0) return;
          }
        else if(call.function!="CREATEINVITEM" && call.function!="EQUIPITEM") return;
        auto* item=vm.find_symbol_by_index(uint32_t(integer(call.args.at(argument))));
        // GameScript also returns without initialization for a missing symbol.
        if(item==nullptr) return;
        const auto previous=item->type()==Type::INSTANCE ? item->get_instance() : nullptr;
        // Use the game's allocation checks, including class/parent validation.
        // Only validate here: the engine call and item constructor run in game.
        vm.allocate_instance<IItem>(item);
        item->set_instance(previous);
        };
      auto event=[&](const DaedalusSymbol& function,bool deferred) {
        flush();
        if(function.has_return()) throw Stop(deferred ? "random_return" : "external_query",function.name());
        Call call{function.name(),arguments(function),deferred};
        if(!deferred) validateItemAllocation(call);
        // These engine paths can write C_NPC fields or execute item scripts.
        // Talent, attitude, appearance-only and routine calls do neither.
        if(deferred || (boundNpc(call.args.at(0)) &&
            (call.function=="EQUIPITEM" || call.function=="CREATEINVITEM" ||
             (call.function=="CREATEINVITEMS" && integer(call.args.at(2))>0) ||
             (call.function=="MDL_SETVISUALBODY" && integer(call.args.at(7))>0) ||
             (call.function=="NPC_SETTOFIGHTMODE" && integer(call.args.at(1))>=0) ||
             call.function=="NPC_SETTOFISTMODE"))) engineState=true;
        result.definition.steps.emplace_back(std::move(call));
        };
      for(const auto& [address,index]:analysis.functions) {
        (void)address;
        auto* function=vm.find_symbol_by_index(index);
        require(function!=nullptr,"missing script function symbol "+std::to_string(index));
        if(!overridden(*function)) continue;
        vm.override_function_custom(*function,[&,function](DaedalusVm&) {
          // Validate only helpers reached by this constructor, before replacing
          // their body with a deferred call. Unrelated random functions may read globals.
          if(analysis.random.count(function->address())) validateDeferred(*function);
          event(*function,true);
          });
        }
      vm.register_default_external_custom([&](DaedalusVm&,DaedalusSymbol& function) {
        if(!engineCalls.count(function.name())) throw Stop("unsupported_external",function.name());
        event(function,false);
        });
      try {
        vm.init_instance(npc,root);
        flush();
        }
      catch(const DaedalusScriptError& error) { result.reason="execution_error"; result.detail=error.what(); result.definition.steps.clear(); }
      return result;
      }
    const NpcSnapshot::Program& program;
    Analysis analysis;
  };

std::vector<uint32_t> corpus(const NpcSnapshot::Program& program) {
  auto script=DaedalusScript::from_program(program.data);
  std::vector<uint32_t> ids;
  if(script.find_symbol_by_name("C_NPC")) script.enumerate_instances_by_class_name("C_NPC",[&](DaedalusSymbol& symbol) {
    if(symbol.is_const() && symbol.address()>0) ids.push_back(symbol.index());
    });
  std::sort(ids.begin(),ids.end(),[&](uint32_t a,uint32_t b) { return program.data.symbols[a].name<program.data.symbols[b].name; });
  return ids;
  }
Json text(std::string_view value,Allocator& allocator) { return Json(value.data(),rapidjson::SizeType(value.size()),allocator); }
void put(Json& json,const char* name,Json value,Allocator& allocator) { json.AddMember(text(name,allocator),value,allocator); }
std::string bytes(std::string_view value) {
  std::string encoded;
  for(auto byte:value) {
    const auto character=uint8_t(byte);
    if(character<128) encoded+=char(character);
    else { encoded+=char(0xc0|(character>>6)); encoded+=char(0x80|(character&63)); }
    }
  return encoded;
  }
Json valueJson(const Value& value,Allocator& allocator) {
  Json json(rapidjson::kObjectType);
  const char* type=nullptr;
  Json data;
  switch(value.kind) {
    case Value::Kind::Int: type="int"; data.SetInt(value.integer); break;
    case Value::Kind::Float: type="float"; data.SetDouble(value.real); break;
    case Value::Kind::String: type="string"; data=text(bytes(value.text),allocator); break;
    case Value::Kind::Symbol: type="symbol"; data=text(value.text,allocator); break;
    case Value::Kind::Self: type="self"; break;
    case Value::Kind::Null: type="null"; break;
    }
  put(json,"type",text(type,allocator),allocator);
  if(value.kind!=Value::Kind::Self && value.kind!=Value::Kind::Null) put(json,"value",std::move(data),allocator);
  return json;
  }
Json recordJson(const std::string& symbol,const Capture& capture,Allocator& allocator) {
  Json json(rapidjson::kObjectType),steps(rapidjson::kArrayType);
  put(json,"symbol",text(symbol,allocator),allocator);
  put(json,"status",text(capture.reason.empty() ? "captured" : "uncaptured",allocator),allocator);
  if(!capture.reason.empty()) {
    put(json,"reason",text(capture.reason,allocator),allocator);
    put(json,"detail",text(capture.detail,allocator),allocator);
    }
  for(const auto& step:capture.definition.steps) {
    Json value(rapidjson::kObjectType);
    if(const auto* field=std::get_if<Field>(&step)) {
      put(value,"field",text(field->binding,allocator),allocator);
      put(value,"index",Json(field->index),allocator);
      put(value,"value",valueJson(field->value,allocator),allocator);
      }
    else {
      const auto& call=std::get<Call>(step);
      Json arguments(rapidjson::kArrayType);
      for(const auto& argument:call.args) arguments.PushBack(valueJson(argument,allocator),allocator);
      put(value,"function",text(call.function,allocator),allocator);
      put(value,"arguments",std::move(arguments),allocator);
      put(value,"deferred",Json(call.deferred),allocator);
      }
    steps.PushBack(value,allocator);
    }
  put(json,"steps",std::move(steps),allocator);
  return json;
  }
Json envelope(const NpcSnapshot::Program& program,Allocator& allocator) {
  Json json(rapidjson::kObjectType);
  put(json,"format",text("gothic-npc-definitions",allocator),allocator);
  put(json,"version",Json(1),allocator);
  put(json,"program_name",text(program.name,allocator),allocator);
  put(json,"program_sha256",text(program.sha256,allocator),allocator);
  put(json,"string_encoding",text("unicode-byte-mapping",allocator),allocator);
  return json;
  }
void difference(const Json& expected,const Json& actual,const std::string& path,int& errors) {
  if(expected==actual) return;
  if(expected.IsObject() && actual.IsObject()) {
    for(auto it=expected.MemberBegin();it!=expected.MemberEnd();++it) {
      auto found=actual.FindMember(it->name);
      if(found==actual.MemberEnd()) { ++errors; std::cerr<<path<<'.'<<it->name.GetString()<<": missing field\n"; }
      else difference(it->value,found->value,path+"."+it->name.GetString(),errors);
      }
    for(auto it=actual.MemberBegin();it!=actual.MemberEnd();++it)
      if(!expected.HasMember(it->name)) { ++errors; std::cerr<<path<<'.'<<it->name.GetString()<<": unexpected field\n"; }
    }
  else if(expected.IsArray() && actual.IsArray()) {
    if(expected.Size()!=actual.Size()) { ++errors; std::cerr<<path<<": count differs\n"; }
    for(rapidjson::SizeType i=0;i<std::min(expected.Size(),actual.Size());++i)
      difference(expected[i],actual[i],path+"["+std::to_string(i)+"]",errors);
    }
  else { ++errors; std::cerr<<path<<": value differs from fresh constructor\n"; }
  }
}

NpcSnapshot::NpcCounts NpcSnapshot::writeNpcs(const Program& program,const fs::path& file) {
  fs::create_directories(file.parent_path());
  rapidjson::Document storage;
  auto& allocator=storage.GetAllocator();
  auto json=envelope(program,allocator);
  Json records(rapidjson::kArrayType);
  Runner runner(program);
  NpcCounts counts;
  for(auto id:corpus(program)) {
    auto capture=runner.run(id);
    ++counts.total;
    if(capture.reason.empty()) ++counts.captured;
    records.PushBack(recordJson(program.data.symbols[id].name,capture,allocator),allocator);
    }
  put(json,"records",std::move(records),allocator);
  rapidjson::StringBuffer buffer;
  rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
  require(json.Accept(writer),"cannot encode NPC definitions: "+file.string());
  require(!fs::is_symlink(file),"NPC definitions output is a symlink: "+file.string());
  std::ofstream output(file,std::ios::binary);
  output.write(buffer.GetString(),std::streamsize(buffer.GetSize())); output.put('\n');
  require(bool(output),"cannot write NPC definitions: "+file.string());
  return counts;
  }

int NpcSnapshot::verifyNpcs(const Program& program,const fs::path& file) {
  std::ifstream input(file,std::ios::binary);
  require(bool(input),"cannot read NPC definitions: "+file.string());
  const std::string bytes{std::istreambuf_iterator<char>(input),{}};
  rapidjson::Document json;
  json.Parse<rapidjson::kParseFullPrecisionFlag | rapidjson::kParseValidateEncodingFlag>(bytes.data(),bytes.size());
  require(!json.HasParseError() && json.IsObject(),"invalid NPC definitions JSON: "+file.string());
  auto& allocator=json.GetAllocator();
  const auto expected=envelope(program,allocator);
  int errors=0;
  for(auto it=expected.MemberBegin();it!=expected.MemberEnd();++it) {
    require(json.HasMember(it->name),file.string()+": missing "+it->name.GetString());
    difference(it->value,json[it->name],file.string()+"."+it->name.GetString(),errors);
    }
  require(json.HasMember("records") && json["records"].IsArray(),file.string()+": expected records array");
  const auto& records=json["records"];
  const auto ids=corpus(program);
  require(records.Size()==ids.size(),file.string()+": NPC record count differs");
  Runner runner(program);
  for(size_t i=0;i<ids.size();++i) {
    const auto& name=program.data.symbols[ids[i]].name;
    auto capture=runner.run(ids[i]);
    difference(recordJson(name,capture,allocator),records[rapidjson::SizeType(i)],"npcs."+name,errors);
    }
  return errors;
  }

int BaseImport::convertNpcDefinitions(const fs::path& base) {
  const auto start=std::chrono::steady_clock::now();
  const auto index=ProgramData::readIndex(base/"programs");
  size_t total=0,captured=0,programs=0;
  for(const auto& [name,sha]:index.programs) {
    NpcSnapshot::Program program;
    program.name=name;
    program.data=ProgramData::readProgram(index.dir/(name+".json"),&program.sha256);
    if(program.sha256!=sha) throw std::runtime_error("NPC definitions: program SHA differs: "+name);
    if(corpus(program).empty()) continue;
    ++programs;
    const auto file=base/"definitions"/sha/"npcs.json";
    const auto counts=NpcSnapshot::writeNpcs(program,file);
    total+=counts.total; captured+=counts.captured;
    if(NpcSnapshot::verifyNpcs(program,file)!=0)
      throw std::runtime_error("NPC definitions: constructor comparison failed: "+name);
    }
  const auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();
  std::cout<<"written and verified "<<programs<<" NPC definition programs; "<<total<<" NPCs, "<<captured
           <<" captured, "<<(total-captured)<<" uncaptured; 0 failed ("<<ms<<" ms)\n";
  return 0;
  }

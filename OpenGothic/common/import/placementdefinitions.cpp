#include "baseimport.h"
#include "npcsnapshot.h"
#include "definitionjson.h"
#include "asset/placementdefinitions.h"
#include "daedalus/classes.h"
#include "daedalus/vm.h"
#include "game/definitions/gameplaydefinitions.h"
#include "programdata.h"

#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <optional>
#include <set>
#include <stdexcept>

namespace fs = std::filesystem;
namespace {
using namespace daedalus;
using namespace PlacementDefinitions;
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
  std::string reason;
  };
// Known placement effects. Queries and unknown engine APIs stay in their script helper.
const std::set<std::string> engineCalls={
  "WLD_INSERTNPC","WLD_INSERTITEM","WLD_ASSIGNROOMTOGUILD","WLD_SETOBJECTROUTINE","WLD_SETMOBROUTINE",
  "WLD_SETTIME","WLD_SETGUILDATTITUDE","WLD_EXCHANGEGUILDATTITUDES","PLAYVIDEO",
  "GAME_INITGERMAN","GAME_INITENGLISH"};

const DaedalusSymbol* localOwner(const DaedalusSymbol& symbol,const DaedalusVm& vm) {
  const auto* parent=vm.find_symbol_by_index(symbol.parent());
  if(parent && parent->type()==Type::FUNCTION) return parent;
  const auto dot=symbol.name().find('.');
  const auto* owner=dot==std::string::npos ? nullptr : vm.find_symbol_by_name(symbol.name().substr(0,dot));
  return owner && owner->type()==Type::FUNCTION ? owner : nullptr;
  }
bool mutableGlobal(const DaedalusSymbol& symbol,const DaedalusVm& vm) {
  return !(symbol.is_const() || symbol.is_member() || localOwner(symbol,vm) ||
           (symbol.is_generated() && symbol.type()==Type::STRING));
  }
Value reference(const DaedalusScript& script,int32_t index) {
  Value value;
  if(index==0) value.kind=Value::Kind::Null;
  else if(index<0) value.integer=index;
  else {
    const auto* symbol=script.find_symbol_by_index(uint32_t(index));
    require(symbol!=nullptr,"invalid symbol reference "+std::to_string(index));
    value.kind=Value::Kind::Symbol; value.text=symbol->name();
    }
  return value;
  }

// Prove that a result remains below every subsequent operand until the void
// frame returns. Unsupported instructions retain the original VM path.
bool discardedResult(const DaedalusScript& script,uint32_t pc) {
  uint32_t depth=0;
  while(pc<script.size()) {
    const auto instruction=script.instruction_at(pc);
    switch(instruction.op) {
      case Op::PUSHI:
      case Op::PUSHV:
      case Op::PUSHVI:
      case Op::PUSHVV: ++depth; break;
      case Op::MOVI:
      case Op::MOVF:
      case Op::MOVS:
        if(depth<2) return false;
        depth-=2;
        break;
      case Op::BE: {
        const auto* function=script.find_symbol_by_index(instruction.symbol);
        if(function==nullptr || depth<function->count()) return false;
        depth-=function->count();
        if(function->has_return()) ++depth;
        break;
        }
      case Op::RSR: return true;
      default: return false;
      }
    pc+=instruction.size;
    }
  return false;
  }
struct Capture {
  Definition definition;
  std::string reason,detail;
  std::map<uint32_t,std::string> deferred;
  };

class Runner {
  public:
    explicit Runner(const NpcSnapshot::Program& program):program(program) {}
    Capture run(uint32_t id) const {
      std::map<uint32_t,std::string> deferred;
      try {
        for(;;) {
          try {
            auto result=capture(id,deferred);
            result.deferred=std::move(deferred);
            return result;
            }
          catch(const Defer& helper) {
            require(deferred.emplace(helper.address,helper.reason).second,"placement dependency replay did not advance");
            }
          catch(const Stop& error) {
            Capture result;
            result.reason=error.reason; result.detail=error.detail;
            return result;
            }
          }
        }
      catch(const std::exception& error) {
        throw std::runtime_error("placement definitions: "+program.data.symbols.at(id).name+": "+error.what());
        }
      }
  private:
    Capture capture(uint32_t id,const std::map<uint32_t,std::string>& deferred) const {
      Capture result;
      auto script=DaedalusScript::from_program(program.data);
      if(script.find_symbol_by_name("C_NPC")) INpc::register_(script);
      if(script.find_symbol_by_name("C_ITEM")) IItem::register_(script);
      DaedalusVm vm(std::move(script));
      const auto* root=vm.find_symbol_by_index(id);
      require(root!=nullptr,"missing placement symbol "+std::to_string(id));
      if(root->count()!=0 || root->has_return()) throw Stop("unsupported_signature",root->name());
      struct Frame {
        const DaedalusSymbol* function;
        std::set<uint32_t> assigned;
        };
      std::vector<Frame> frames;
      struct GlobalTarget {
        DaedalusSymbol* symbol;
        uint16_t index;
        bool written=false;
        };
      std::optional<GlobalTarget> globalTarget;
      auto validateDeferred=[&](const DaedalusSymbol& helper) {
        std::map<uint32_t,const DaedalusSymbol*> functions;
        for(const auto& symbol:vm.symbols())
          if(!symbol.is_external() && symbol.address()<vm.size() &&
             (symbol.type()==Type::FUNCTION || symbol.type()==Type::PROTOTYPE ||
              (symbol.type()==Type::INSTANCE && symbol.is_const() && symbol.address()>0 &&
               symbol.name().find('.')==std::string::npos)))
            functions.emplace(symbol.address(),&symbol);
        std::set<uint32_t> visited;
        std::vector<uint32_t> todo{helper.address()};
        std::set<const DaedalusSymbol*> owners;
        std::vector<const DaedalusSymbol*> locals;
        while(!todo.empty()) {
          const auto address=todo.back(); todo.pop_back();
          if(!visited.insert(address).second) continue;
          const auto function=functions.find(address);
          require(function!=functions.end(),"missing deferred script function at "+std::to_string(address));
          owners.insert(function->second);
          const auto next=std::next(function);
          const auto end=next==functions.end() ? vm.size() : next->first;
          for(auto pc=address;pc<end;) {
            const auto instruction=vm.instruction_at(pc);
            if(instruction.op==Op::BL) todo.push_back(instruction.address);
            if(instruction.op==Op::PUSHV || instruction.op==Op::PUSHVI || instruction.op==Op::PUSHVV ||
               instruction.op==Op::GMOVI) {
              const auto* symbol=vm.find_symbol_by_index(instruction.symbol);
              require(symbol!=nullptr,"missing deferred symbol "+std::to_string(instruction.symbol));
              if(!symbol->is_const() && localOwner(*symbol,vm)) locals.push_back(symbol);
              }
            pc+=instruction.size;
            }
          }
        // Assignments in the flattened caller are absent during playback. A helper
        // that refers to its caller's locals cannot safely cross that boundary.
        for(const auto* symbol:locals)
          if(!owners.contains(localOwner(*symbol,vm)))
            throw Stop("deferred_local_dependency",helper.name()+": "+symbol->name());
        };
      auto fail=[&](const std::string& reason,const std::string& detail) -> void {
        const auto where=detail+" at pc="+std::to_string(vm.pc())+" opcode="+
                         std::to_string(uint32_t(vm.instruction_at(vm.pc()).op));
        // Keep the nearest void helper at its call boundary. A value-returning
        // helper needs its nearest void caller so no computed result is baked in.
        for(size_t i=frames.size();i>1;--i) {
          const auto* helper=frames[i-1].function;
          if(helper->type()!=Type::FUNCTION || helper->has_return()) continue;
          validateDeferred(*helper);
          throw Defer{helper->address(),reason+": "+where};
          }
        throw Stop(reason,where);
        };
      auto flushGlobal=[&] {
        if(globalTarget && globalTarget->written) {
          GlobalWrite write{globalTarget->symbol->name(),globalTarget->index,{}};
          switch(globalTarget->symbol->type()) {
            case Type::INT: write.value.integer=globalTarget->symbol->get_int(write.index); break;
            case Type::FLOAT:
              write.value.kind=Value::Kind::Float;
              write.value.real=globalTarget->symbol->get_float(write.index);
              break;
            case Type::STRING:
              write.value.kind=Value::Kind::String;
              write.value.text=globalTarget->symbol->get_string(write.index);
              break;
            default: throw std::runtime_error("placement global type changed after assignment");
            }
          result.definition.steps.emplace_back(std::move(write));
          }
        globalTarget.reset();
        };
      vm.register_exception_handler([&](DaedalusVm&,const DaedalusScriptError& error,const DaedalusInstruction&)
                                      -> DaedalusVmExceptionStrategy {
        fail("script_error",error.what());
        return DaedalusVmExceptionStrategy::FAIL;
        });
      vm.observe_calls([&](const DaedalusSymbol& function) {
        if(!frames.empty() && deferred.contains(function.address())) return;
        frames.push_back({&function,{}});
        });
      vm.observe_reads([&](const DaedalusSymbol& symbol) {
        if(mutableGlobal(symbol,vm)) fail("game_state",symbol.name());
        if(symbol.is_member()) fail("engine_state_read",symbol.name());
        if(const auto* owner=localOwner(symbol,vm); owner && !symbol.is_const()) {
          // Locals persist in Daedalus symbols between calls. Only a value assigned
          // in this invocation can be baked into a placement argument or branch.
          if(symbol.count()!=1) fail("unsupported_local_array",symbol.name());
          const auto frame=std::find_if(frames.rbegin(),frames.rend(),[&](const auto& frame) {
            return frame.function==owner;
            });
          if(frame==frames.rend() || !frame->assigned.contains(symbol.index()))
            fail("local_state",symbol.name()+" before assignment");
          }
        });
      vm.observe_writes([&](const DaedalusSymbol& symbol) {
        if(mutableGlobal(symbol,vm)) {
          if(!globalTarget || globalTarget->symbol!=&symbol) fail("game_state_write",symbol.name());
          // The VM has consumed and checked the RHS, but has not stored it yet.
          // Read its exact typed value at the next instruction boundary.
          globalTarget->written=true;
          return;
          }
        if(symbol.is_member()) fail("engine_state_write",symbol.name());
        if(const auto* owner=localOwner(symbol,vm)) {
          const auto frame=std::find_if(frames.rbegin(),frames.rend(),[&](const auto& frame) {
            return frame.function==owner;
            });
          if(frame==frames.rend()) fail("local_state_write",symbol.name());
          frame->assigned.insert(symbol.index());
          }
        });
      size_t instructions=0;
      vm.observe_instructions([&](const DaedalusInstruction& instruction) {
        flushGlobal();
        if(++instructions>1000000) fail("execution_limit",root->name());
        if(instruction.op==Op::MOVI || instruction.op==Op::MOVF || instruction.op==Op::MOVS) {
          try {
            const auto& target=vm.stack_frame(0);
            if(target.reference) {
              auto* symbol=std::get<DaedalusSymbol*>(target.value);
              if(symbol && mutableGlobal(*symbol,vm)) {
                const auto type=instruction.op==Op::MOVI ? Type::INT :
                                instruction.op==Op::MOVF ? Type::FLOAT : Type::STRING;
                if(symbol->type()!=type || target.index>=symbol->count())
                  fail("game_state_write",symbol->name()+": unsupported type or array index");
                globalTarget=GlobalTarget{symbol,target.index};
                }
              }
            }
          catch(const DaedalusScriptError& error) { fail("script_error",error.what()); }
          }
        // GMOVI changes the member context seen by engine calls. Preserve its
        // enclosing helper because placement steps do not represent that context.
        if(instruction.op==Op::GMOVI) fail("instance_context",std::to_string(instruction.symbol));
        if(instruction.op==Op::RSR) {
          require(!frames.empty(),"placement capture call stack is empty");
          frames.pop_back();
          }
        });
      auto arguments=[&](const DaedalusSymbol& function) {
        std::vector<Value> values;
        for(uint32_t i=function.count();i>0;--i) {
          const auto* parameter=vm.find_symbol_by_index(function.index()+i);
          require(parameter!=nullptr,"missing parameter of "+function.name());
          Value value;
          switch(parameter->type()) {
            case Type::INSTANCE: {
              if(vm.top_is_reference()) {
                auto [symbol,index,context]=vm.pop_reference();
                (void)index; (void)context;
                require(symbol!=nullptr,"missing instance argument symbol for "+function.name());
                if(mutableGlobal(*symbol,vm)) fail("game_state",symbol->name());
                if(localOwner(*symbol,vm)) fail("instance_local",symbol->name());
                value.kind=Value::Kind::Symbol; value.text=symbol->name();
                }
              else {
                const auto instance=vm.pop_instance();
                if(instance) value=reference(vm,int32_t(instance->symbol_index()));
                else value.kind=Value::Kind::Null;
                }
              break;
              }
            case Type::STRING: value.kind=Value::Kind::String; value.text=vm.pop_string(); break;
            case Type::FLOAT: value.kind=Value::Kind::Float; value.real=vm.pop_float(); break;
            case Type::FUNCTION:
            case Type::INT: {
              const auto integer=vm.pop_int();
              const bool symbol=parameter->type()==Type::FUNCTION ||
                  ((function.name()=="WLD_INSERTNPC" || function.name()=="WLD_INSERTITEM" || function.name()=="B_KILLNPC") && i==1);
              if(symbol) value=reference(vm,integer);
              else value.integer=integer;
              break;
              }
            default: fail("unsupported_parameter",function.name()+"."+parameter->name());
            }
          values.push_back(std::move(value));
          }
        std::reverse(values.begin(),values.end());
        return values;
        };
      for(const auto& [address,reason]:deferred) {
        (void)reason;
        const auto* function=vm.find_symbol_by_address(address);
        require(function!=nullptr,"missing deferred helper at "+std::to_string(address));
        vm.override_function_custom(*function,[&,function](DaedalusVm&) {
          result.definition.steps.emplace_back(Call{function->name(),arguments(*function),true});
          });
        }
      vm.register_default_external_custom([&](DaedalusVm&,DaedalusSymbol& function) {
        if(function.has_return()) {
          const auto instruction=vm.instruction_at(vm.pc());
          const auto next=vm.pc()+instruction.size;
          // Never synthesize values that can reach arguments, fields or control flow.
          const bool discarded=engineCalls.contains(function.name()) && function.rtype()==Type::INT &&
              !frames.empty() && !frames.back().function->has_return() && instruction.op==Op::BE &&
              discardedResult(vm,next);
          if(!discarded) fail("external_query",function.name());
          }
        if(!engineCalls.contains(function.name())) fail("unsupported_external",function.name());
        Call call{function.name(),arguments(function),false};
        if(call.function=="WLD_INSERTNPC" || call.function=="WLD_INSERTITEM") {
          const auto& value=call.args.at(0);
          auto* symbol=value.kind==Value::Kind::Symbol ? vm.find_symbol_by_name(value.text) :
                       value.integer>0 ? vm.find_symbol_by_index(uint32_t(value.integer)) : nullptr;
          if(symbol && !(call.function=="WLD_INSERTITEM" && call.args.at(1).text.empty())) {
            const auto previous=symbol->type()==Type::INSTANCE ? symbol->get_instance() : nullptr;
            // Allocation checks match the live creation path without running its
            // constructor. Invalid classes are lenient script errors in the VM.
            if(call.function=="WLD_INSERTNPC") vm.allocate_instance<INpc>(symbol);
            else vm.allocate_instance<IItem>(symbol);
            symbol->set_instance(previous);
            }
          }
        result.definition.steps.emplace_back(std::move(call));
        if(function.has_return()) vm.push_int(0);
        });
      vm.call_function(root);
      return result;
      }
    const NpcSnapshot::Program& program;
  };

std::vector<uint32_t> corpus(const NpcSnapshot::Program& program,const fs::path& worldsDirectory) {
  std::set<std::string> roots{"STARTUP_GLOBAL"};
  for(const auto& world:fs::directory_iterator(worldsDirectory))
    if(world.is_directory()) roots.insert("STARTUP_"+world.path().filename().string());
  std::vector<uint32_t> ids;
  for(uint32_t i=0;i<program.data.symbols.size();++i) {
    const auto& symbol=program.data.symbols[i];
    if(symbol.type==uint32_t(Type::FUNCTION) && !(symbol.flags&DaedalusSymbolFlag::EXTERNAL) &&
       roots.contains(symbol.name)) ids.push_back(i);
    }
  std::sort(ids.begin(),ids.end(),[&](uint32_t a,uint32_t b) { return program.data.symbols[a].name<program.data.symbols[b].name; });
  return ids;
  }
using DefinitionJson::text;
using DefinitionJson::put;
using DefinitionJson::encode;
Json valueJson(const Value& value,Allocator& allocator) {
  Json json(rapidjson::kObjectType),data;
  const char* type=nullptr;
  switch(value.kind) {
    case Value::Kind::Int: type="int"; data.SetInt(value.integer); break;
    case Value::Kind::Float: type="float"; data.SetDouble(value.real); break;
    case Value::Kind::String: type="string"; data=text(encode(value.text),allocator); break;
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
    Json jsonStep(rapidjson::kObjectType);
    if(const auto* call=std::get_if<Call>(&step)) {
      Json arguments(rapidjson::kArrayType);
      for(const auto& argument:call->args) arguments.PushBack(valueJson(argument,allocator),allocator);
      put(jsonStep,"function",text(call->function,allocator),allocator);
      put(jsonStep,"arguments",std::move(arguments),allocator);
      put(jsonStep,"deferred",Json(call->deferred),allocator);
      }
    else {
      const auto& write=std::get<GlobalWrite>(step);
      put(jsonStep,"global",text(write.symbol,allocator),allocator);
      put(jsonStep,"index",Json(write.index),allocator);
      put(jsonStep,"value",valueJson(write.value,allocator),allocator);
      }
    steps.PushBack(jsonStep,allocator);
    }
  put(json,"steps",std::move(steps),allocator);
  return json;
  }
template<size_t N>
Json lateChapterGroups(const DaedalusScript& script,std::string_view root,const std::array<uint32_t,N>& offsets,
                       const std::array<size_t,N>& sizes,Allocator& allocator) {
  const auto* chapter=script.find_symbol_by_name(root);
  require(chapter && chapter->type()==Type::FUNCTION && !chapter->is_external(),"missing placement chapter "+std::string(root));
  Json groups(rapidjson::kArrayType);
  for(size_t i=0;i<N;++i) {
    auto pc=chapter->address()+offsets[i];
    Json rows(rapidjson::kArrayType);
    for(size_t j=0;j<sizes[i];++j) {
      require(pc<script.size() && script.size()-pc>=15,"truncated chapter placement in "+std::string(root));
      const auto instance=script.instruction_at(pc);
      const auto point=script.instruction_at(pc+5);
      const auto insert=script.instruction_at(pc+10);
      const auto* what=script.find_symbol_by_index(uint32_t(instance.immediate));
      const auto* where=script.find_symbol_by_index(point.symbol);
      const auto* external=script.find_symbol_by_index(insert.symbol);
      require(instance.op==Op::PUSHI && what && what->type()==Type::INSTANCE && what->is_const() &&
              point.op==Op::PUSHV && where && where->type()==Type::STRING && where->is_const() &&
              insert.op==Op::BE && external && (external->name()=="WLD_INSERTNPC" || external->name()=="WLD_INSERTITEM"),
              "unexpected literal chapter placement in "+std::string(root));
      Json row(rapidjson::kArrayType);
      row.PushBack(Json(what->index()),allocator);
      row.PushBack(Json(where->index()),allocator);
      row.PushBack(Json(external->index()),allocator);
      rows.PushBack(row,allocator);
      pc+=15;
      }
    groups.PushBack(rows,allocator);
    }
  return groups;
  }
Json envelope(const NpcSnapshot::Program& program,Allocator& allocator) {
  Json json(rapidjson::kObjectType);
  const bool stock=program.sha256==GameplayDefinitions::stockProgram;
  put(json,"format",text("gothic-placement-definitions",allocator),allocator);
  put(json,"version",Json(stock ? 2 : 1),allocator);
  put(json,"program_name",text(program.name,allocator),allocator);
  put(json,"program_sha256",text(program.sha256,allocator),allocator);
  put(json,"string_encoding",text("unicode-byte-mapping",allocator),allocator);
  if(stock) {
    const auto script=DaedalusScript::from_program(program.data);
    // Offsets of the literal insert calls within the verified stock functions.
    constexpr std::array<uint32_t,6> offsets4{82,328,1281,1552,2923,3330};
    constexpr std::array<uint32_t,7> offsets5{160,191,206,482,554,1000,1772};
    Json chapters(rapidjson::kObjectType);
    put(chapters,"chapter_4",lateChapterGroups(script,"B_ENTER_NEWWORLD_KAPITEL_4",offsets4,
                                              KhorinisLateChapters::chapter4Sizes,allocator),allocator);
    put(chapters,"chapter_5",lateChapterGroups(script,"B_ENTER_NEWWORLD_KAPITEL_5",offsets5,
                                              KhorinisLateChapters::chapter5Sizes,allocator),allocator);
    put(json,"khorinis_late_chapters",std::move(chapters),allocator);
    }
  return json;
  }
using DefinitionJson::difference;
}

NpcSnapshot::PlacementCounts NpcSnapshot::writePlacements(const Program& program,const fs::path& file,
                                                        const fs::path& worldsDirectory) {
  fs::create_directories(file.parent_path());
  rapidjson::Document storage;
  auto& allocator=storage.GetAllocator();
  auto json=envelope(program,allocator);
  Json records(rapidjson::kArrayType);
  Runner runner(program);
  PlacementCounts counts;
  for(auto id:corpus(program,worldsDirectory)) {
    const auto capture=runner.run(id);
    ++counts.total;
    if(capture.reason.empty()) ++counts.captured;
    counts.steps+=capture.definition.steps.size();
    for(const auto& step:capture.definition.steps)
      if(const auto* call=std::get_if<Call>(&step); call && call->deferred) ++counts.deferred;
    const auto& name=program.data.symbols[id].name;
    if(!capture.reason.empty()) std::cout<<"placement "<<name<<": uncaptured "<<capture.reason<<": "<<capture.detail<<'\n';
    for(const auto& [address,reason]:capture.deferred)
      for(const auto& symbol:program.data.symbols)
        if(symbol.type==uint32_t(Type::FUNCTION) && !(symbol.flags&DaedalusSymbolFlag::EXTERNAL) && uint32_t(symbol.address)==address) {
          std::cout<<"placement "<<name<<": deferred "<<symbol.name<<": "<<reason<<'\n';
          break;
          }
    records.PushBack(recordJson(name,capture,allocator),allocator);
    }
  put(json,"records",std::move(records),allocator);
  rapidjson::StringBuffer buffer;
  rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
  require(json.Accept(writer),"cannot encode placement definitions: "+file.string());
  require(!fs::is_symlink(file),"placement definitions output is a symlink: "+file.string());
  std::ofstream output(file,std::ios::binary);
  output.write(buffer.GetString(),std::streamsize(buffer.GetSize())); output.put('\n');
  require(bool(output),"cannot write placement definitions: "+file.string());
  return counts;
  }

int NpcSnapshot::verifyPlacements(const Program& program,const fs::path& file,const fs::path& worldsDirectory) {
  std::ifstream input(file,std::ios::binary);
  require(bool(input),"cannot read placement definitions: "+file.string());
  const std::string bytes{std::istreambuf_iterator<char>(input),{}};
  rapidjson::Document json;
  json.Parse<rapidjson::kParseFullPrecisionFlag | rapidjson::kParseValidateEncodingFlag>(bytes.data(),bytes.size());
  require(!json.HasParseError() && json.IsObject(),"invalid placement definitions JSON: "+file.string());
  auto& allocator=json.GetAllocator();
  const auto expected=envelope(program,allocator);
  int errors=0;
  for(auto it=expected.MemberBegin();it!=expected.MemberEnd();++it) {
    require(json.HasMember(it->name),file.string()+": missing "+it->name.GetString());
    difference(it->value,json[it->name],file.string()+"."+it->name.GetString(),errors);
    }
  require(json.HasMember("records") && json["records"].IsArray(),file.string()+": expected records array");
  const auto& records=json["records"];
  const auto ids=corpus(program,worldsDirectory);
  require(records.Size()==ids.size(),file.string()+": placement record count differs");
  Runner runner(program);
  for(size_t i=0;i<ids.size();++i) {
    const auto& name=program.data.symbols[ids[i]].name;
    difference(recordJson(name,runner.run(ids[i]),allocator),records[rapidjson::SizeType(i)],"placement."+name,errors);
    }
  return errors;
  }

int BaseImport::convertPlacementDefinitions(const fs::path& base) {
  const auto start=std::chrono::steady_clock::now();
  const auto index=ProgramData::readIndex(base/"programs");
  size_t total=0,captured=0,steps=0,deferred=0,programs=0;
  for(const auto& [name,sha]:index.programs) {
    NpcSnapshot::Program program;
    program.name=name;
    program.data=ProgramData::readProgram(index.dir/(name+".json"),&program.sha256);
    if(program.sha256!=sha) throw std::runtime_error("placement definitions: program SHA differs: "+name);
    if(corpus(program,base/"worlds").empty()) continue;
    ++programs;
    const auto file=base/"definitions"/sha/"placement.json";
    const auto counts=NpcSnapshot::writePlacements(program,file,base/"worlds");
    total+=counts.total; captured+=counts.captured; steps+=counts.steps; deferred+=counts.deferred;
    if(NpcSnapshot::verifyPlacements(program,file,base/"worlds")!=0)
      throw std::runtime_error("placement definitions: fresh VM comparison failed: "+name);
    }
  const auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();
  std::cout<<"written and verified "<<programs<<" placement programs; "<<total<<" functions, "<<captured
           <<" captured, "<<(total-captured)<<" uncaptured, "<<steps<<" steps, "<<deferred<<" deferred; 0 failed ("<<ms<<" ms)\n";
  return 0;
  }

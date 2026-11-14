#include "npcsnapshot.h"
#include "daedalus/classes.h"
#include "npcsnapshotfields.h"
#include "daedalus/vm.h"
#include "programdata.h"
#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>

using namespace daedalus;
namespace fs = std::filesystem;
namespace {
using J = rapidjson::Value;
using A = rapidjson::Document::AllocatorType;
using Type = DaedalusDataType;
using Op = DaedalusOpcode;
namespace Flag = DaedalusSymbolFlag;

void require(bool ok, const std::string& why) {
  if(!ok) throw std::runtime_error(why);
  }
J text(std::string_view s, A& a) { return J(s.data(),rapidjson::SizeType(s.size()),a); }
void put(J& j, const char* key, J v, A& a) { j.AddMember(text(key,a),v,a); }
std::string string(const J& j) {
  require(j.IsString(),"expected a string");
  return {j.GetString(),j.GetStringLength()};
  }
const J& at(const J& j, const char* key) {
  require(j.IsObject() && j.HasMember(key),std::string("missing field ")+key);
  return j[key];
  }
// Script strings are byte strings. This reversible Unicode byte mapping preserves even
// undefined code-page bytes, without guessing the language or dropping invalid UTF-8.
std::string encode(std::string_view s) {
  std::string r;
  for(char byte:s) {
    const auto c=uint8_t(byte);
    if(c<128) r+=char(c);
    else { r+=char(0xC0|(c>>6)); r+=char(0x80|(c&63)); }
    }
  return r;
  }
std::string decode(const J& j) {
  auto s=string(j);
  std::string r;
  for(size_t i=0;i<s.size();++i) {
    auto c=uint8_t(s[i]);
    if(c<128) r+=char(c);
    else {
      require((c==0xC2 || c==0xC3) && i+1<s.size() && (uint8_t(s[i+1])&0xC0)==0x80,"invalid script byte string");
      r+=char(((c&3)<<6)|(uint8_t(s[++i])&63));
      }
    }
  return r;
  }
rapidjson::Document readJson(const fs::path& path) {
  std::ifstream f(path,std::ios::binary);
  require(bool(f),"cannot read "+path.string());
  std::string s{std::istreambuf_iterator<char>(f),{}};
  rapidjson::Document d;
  d.Parse<rapidjson::kParseFullPrecisionFlag | rapidjson::kParseValidateEncodingFlag>(s.data(),s.size());
  require(!d.HasParseError(),"invalid JSON: "+path.string());
  return d;
  }
void writeJson(const fs::path& path, const J& j) {
  rapidjson::StringBuffer b;
  rapidjson::PrettyWriter<rapidjson::StringBuffer> w(b);
  require(j.Accept(w),"cannot encode "+path.string());
  require(!fs::is_symlink(path),"snapshot output is a symlink: "+path.string());
  std::ofstream f(path,std::ios::binary);
  f.write(b.GetString(),std::streamsize(b.GetSize())); f.put('\n');
  require(bool(f),"cannot write "+path.string());
  }
J copy(const J& j,A& a) { J r; r.CopyFrom(j,a); return r; }

std::string symbolName(const DaedalusScript& vm,int32_t index) {
  if(index==0) return {};
  auto* s=vm.find_symbol_by_index(uint32_t(index));
  require(s!=nullptr,"invalid symbol reference "+std::to_string(index));
  return s->name();
  }
int32_t symbolIndex(const DaedalusScript& vm,const J& j) {
  if(j.IsNull()) return 0;
  if(j.IsInt() && j.GetInt()==-1) return -1;
  auto name=string(j);
  auto* s=vm.find_symbol_by_name(name);
  require(s!=nullptr,"unknown symbol "+name);
  return int32_t(s->index());
  }

template<class T> J fieldJson(const T& v,bool function,const DaedalusScript& vm,A& a) {
  if constexpr(std::is_array_v<T>) {
    J out(rapidjson::kArrayType);
    for(auto& x:v) out.PushBack(fieldJson(x,function,vm,a),a);
    return out;
    }
  else if constexpr(std::is_same_v<T,std::string>) return text(encode(v),a);
  else {
    if(function) {
      if(int32_t(v)==-1) return J(-1);
      auto name=symbolName(vm,int32_t(v));
      return name.empty() ? J() : text(name,a);
      }
    if constexpr(std::is_floating_point_v<T>) return J(double(v));
    else return J(int32_t(v));
    }
  }
template<class T> void restoreField(T& v,const J& j,bool function,const DaedalusScript& vm) {
  if constexpr(std::is_array_v<T>) {
    require(j.IsArray() && j.Size()==std::extent_v<T>,"array extent differs");
    for(size_t i=0;i<std::extent_v<T>;++i) restoreField(v[i],j[rapidjson::SizeType(i)],function,vm);
    }
  else if constexpr(std::is_same_v<T,std::string>) v=decode(j);
  else {
    if(function) v=T(symbolIndex(vm,j));
    else if constexpr(std::is_floating_point_v<T>) { require(j.IsNumber(),"expected number"); v=T(j.GetDouble()); }
    else { require(j.IsInt(),"expected int32"); v=T(j.GetInt()); }
    }
  }
template<class T> bool equalField(const T& a,const T& b) {
  if constexpr(std::is_array_v<T>) {
    for(size_t i=0;i<std::extent_v<T>;++i) if(!equalField(a[i],b[i])) return false;
    return true;
    }
  else return a==b;
  }
template<class T> J fields(T& instance,const DaedalusScript& vm,A& a) {
  J j(rapidjson::kObjectType);
  snapshotFields(instance,[&](const char* name,auto& value,bool fn,const char*) { put(j,name,fieldJson(value,fn,vm,a),a); });
  return j;
  }
template<class T> void verifyFields(T& expected,const J& j,const DaedalusScript& vm) {
  // Restore an actual native instance; compare every native field, not just keys found in JSON.
  T restored{};
  size_t count=0;
  snapshotFields(restored,[&](const char* name,auto& value,bool fn,const char*) {
    try { restoreField(value,at(j,name),fn,vm); }
    catch(const std::exception& e) { throw std::runtime_error(std::string(name)+": "+e.what()); }
    ++count;
    });
  require(j.MemberCount()==count,"unexpected native field");
  snapshotFields(expected,[&](const char* name,auto& value,bool,const char*) {
    bool equal=false;
    snapshotFields(restored,[&](const char* other,auto& actual,bool,const char*) {
      if constexpr(std::is_same_v<std::remove_reference_t<decltype(value)>,std::remove_reference_t<decltype(actual)>>)
        if(std::string_view(name)==other) equal=equalField(value,actual);
      });
    require(equal,std::string(name)+": value differs from constructor");
    });
  }

struct Stop : std::runtime_error {
  std::string reason,detail;
  Stop(std::string reason,std::string detail):std::runtime_error(reason+": "+detail),reason(std::move(reason)),detail(std::move(detail)) {}
  };

// One rule throughout D1: the transitive BL call graph decides randomness before execution;
// mutable globals are detected on VM value consumption, excluding members, constants, locals,
// generated string literals and the bound SELF. No function name whitelist declares a helper pure.
struct Analysis {
  std::set<uint32_t> random;
  std::map<uint32_t,uint32_t> functions;
  explicit Analysis(const DaedalusScript& vm) {
    for(auto& s:vm.symbols())
      if(!s.is_external() && (s.type()==Type::FUNCTION || s.type()==Type::PROTOTYPE ||
          (s.type()==Type::INSTANCE && s.is_const() && s.address()>0 && s.name().find('.')==std::string::npos)) && s.address()<vm.size())
        functions.emplace(s.address(),s.index());
    std::map<uint32_t,std::set<uint32_t>> calls;
    for(auto it=functions.begin();it!=functions.end();++it) {
      auto end=std::next(it)==functions.end() ? vm.size() : std::next(it)->first;
      for(auto pc=it->first;pc<end;) {
        auto op=vm.instruction_at(pc);
        if(op.op==Op::BL) calls[it->first].insert(op.address);
        if(op.op==Op::BE) {
          auto* ext=vm.find_symbol_by_index(op.symbol);
          if(ext!=nullptr && ext->name()=="HLP_RANDOM") random.insert(it->first);
          }
        pc+=op.size;
        }
      }
    bool changed=true;
    while(changed) {
      changed=false;
      for(auto& [fn,edges]:calls)
        for(auto target:edges)
          if(random.count(target) && random.insert(fn).second) changed=true;
      }
    }
  };

bool mutableGlobal(const DaedalusSymbol& sym,const DaedalusVm& vm) {
  auto* parent=vm.find_symbol_by_index(sym.parent());
  const auto dot=sym.name().find('.');
  const auto* owner=dot==std::string::npos ? nullptr : vm.find_symbol_by_name(sym.name().substr(0,dot));
  bool local=(parent && parent->type()==Type::FUNCTION) || (owner && owner->type()==Type::FUNCTION);
  return !(sym.is_const() || sym.is_member() || local ||
           (sym.is_generated() && sym.type()==Type::STRING) || sym.name()=="SELF");
  }

struct Capture {
  std::shared_ptr<INpc> npc;
  std::shared_ptr<IItem> item;
  std::unique_ptr<DaedalusVm> vm;
  J record{rapidjson::kObjectType};
  };
class Runner {
  public:
    Runner(const NpcSnapshot::Program& p):program(p),analysis(DaedalusScript::from_program(p.data)) {}
    Capture run(uint32_t id,const std::string& kind,A& a) const {
      Capture c;
      auto script=DaedalusScript::from_program(program.data);
      // Only these two classes execute in D1; other classes need no binding.
      if(script.find_symbol_by_name("C_NPC")) INpc::register_(script);
      if(script.find_symbol_by_name("C_ITEM")) IItem::register_(script);
      c.vm=std::make_unique<DaedalusVm>(std::move(script));
      auto& vm=*c.vm;
      auto* root=vm.find_symbol_by_index(id);
      put(c.record,"symbol",text(root->name(),a),a);
      J calls(rapidjson::kArrayType),rows(rapidjson::kArrayType);
      std::string pending;
      size_t instructions=0;
      vm.observe_reads([&](const DaedalusSymbol& sym) {
        if(!mutableGlobal(sym,vm)) return;
        if(kind=="population") { if(pending.empty()) pending=sym.name(); }
        else throw Stop("game_state",sym.name());
        });
      vm.observe_instructions([&](const DaedalusInstruction& op) {
        if(++instructions>1000000) throw Stop("execution_limit",root->name());
        if(!pending.empty() && op.op==Op::BZ) throw Stop("game_state",pending+" before state branch");
        });
      auto arguments=[&](const DaedalusSymbol& fn) {
        J args(rapidjson::kArrayType);
        std::vector<J> values;
        for(uint32_t i=fn.count();i>0;--i) {
          auto* par=vm.find_symbol_by_index(fn.index()+i);
          require(par!=nullptr,"missing parameter of "+fn.name());
          J v;
          switch(par->type()) {
            case Type::INSTANCE: {
              // Preserve an unbound named instance too (pop_instance would turn it into null).
              if(vm.top_is_reference()) {
                auto [sym,index,context]=vm.pop_reference();
                (void)index; (void)context;
                if(mutableGlobal(*sym,vm))
                  throw Stop("game_state",sym->name());
                auto inst=sym->get_instance();
                v=text(sym==vm.global_self() || (inst && vm.global_self() && inst==vm.global_self()->get_instance()) ? "self" : sym->name(),a);
                }
              else {
                auto inst=vm.pop_instance();
                v=inst ? text(vm.global_self() && inst==vm.global_self()->get_instance() ? "self" : symbolName(vm,int32_t(inst->symbol_index())),a) : J();
                }
              break;
              }
            case Type::STRING: v=text(encode(vm.pop_string()),a); break;
            case Type::FLOAT: v=J(double(vm.pop_float())); break;
            case Type::INT:
            case Type::FUNCTION: {
              auto n=vm.pop_int();
              // Daedalus declares instance/state parameters as INT in these engine APIs.
              auto name=fn.name();
              bool ref=par->type()==Type::FUNCTION ||
                  ((name=="EQUIPITEM" || name=="CREATEINVITEM" || name=="CREATEINVITEMS") && i==2) ||
                  ((name=="WLD_INSERTNPC" || name=="WLD_INSERTITEM") && i==1) ||
                  (name=="TA_MIN" && i==6) || (name=="MDL_SETVISUALBODY" && i==8) ||
                  (name=="NPC_SETTOFIGHTMODE" && i==2);
              auto symbol=ref && n>0 ? symbolName(vm,n) : std::string();
              v=ref && n>=0 ? (symbol.empty() ? J() : text(symbol,a)) : J(n);
              break;
              }
            default: throw Stop("unsupported_parameter",fn.name()+"."+par->name());
            }
          values.push_back(std::move(v));
          }
        for(auto it=values.rbegin();it!=values.rend();++it) args.PushBack(*it,a);
        return args;
        };
      auto event=[&](const DaedalusSymbol& fn,bool deferred) {
        J call(rapidjson::kObjectType);
        put(call,"function",text(fn.name(),a),a);
        put(call,"arguments",arguments(fn),a);
        if(!pending.empty()) throw Stop("game_state",pending+" used before engine call");
        if(deferred) put(call,"deferred",J(true),a);
        if(kind=="population") {
          auto caller=analysis.functions.upper_bound(vm.pc());
          if(caller!=analysis.functions.begin()) --caller;
          put(call,"source",text(vm.find_symbol_by_index(caller->second)->name(),a),a);
          }
        return call;
        };
      for(auto address:analysis.random) {
        auto* fn=vm.find_symbol_by_address(address);
        if(fn==nullptr || fn->type()!=Type::FUNCTION) continue;
        vm.override_function_custom(*fn,[&,fn](DaedalusVm&) {
          calls.PushBack(event(*fn,true),a);
          // Never fabricate a random return value that could flow into fields or control flow.
          if(fn->has_return()) throw Stop("random_return",fn->name());
          });
        }
      vm.register_default_external_custom([&](DaedalusVm&,DaedalusSymbol& fn) {
        if(fn.name()=="HLP_RANDOM") throw Stop("random_root",root->name());
        // A value-returning external queries the world. A dummy return would bake game state.
        if(fn.has_return()) throw Stop("external_query",fn.name());
        auto call=event(fn,false);
        if(kind=="routines" && fn.name()=="TA_MIN") {
          auto& args=call["arguments"];
          require(args.Size()==7,"TA_Min signature differs");
          J row(rapidjson::kObjectType),start(rapidjson::kArrayType),end(rapidjson::kArrayType);
          start.PushBack(copy(args[1],a),a).PushBack(copy(args[2],a),a);
          end.PushBack(copy(args[3],a),a).PushBack(copy(args[4],a),a);
          put(row,"start",std::move(start),a); put(row,"end",std::move(end),a);
          put(row,"state",copy(args[5],a),a); put(row,"waypoint",copy(args[6],a),a);
          rows.PushBack(row,a);
          }
        else if(kind!="population" || fn.name()=="WLD_INSERTNPC" || fn.name()=="WLD_INSERTITEM" ||
            fn.name()=="WLD_ASSIGNROOMTOGUILD" || fn.name()=="WLD_SETMOBROUTINE") calls.PushBack(call,a);
        });
      try {
        if(kind=="npcs") { c.npc=std::make_shared<INpc>(); vm.init_instance(c.npc,root); }
        else if(kind=="items") { c.item=std::make_shared<IItem>(); vm.init_instance(c.item,root); }
        else vm.unsafe_call(root);
        if(!pending.empty()) throw Stop("game_state",pending);
        put(c.record,"status",text("captured",a),a);
        }
      catch(const Stop& e) {
        put(c.record,"status",text("uncaptured",a),a);
        put(c.record,"reason",text(e.reason,a),a); put(c.record,"detail",text(e.detail,a),a);
        // Partial native values are not an NPC definition. Population alone keeps its safe prefix.
        c.npc.reset(); c.item.reset();
        if(kind!="population") { calls.Clear(); rows.Clear(); }
        }
      catch(const std::exception& e) {
        put(c.record,"status",text("uncaptured",a),a);
        put(c.record,"reason",text("execution_error",a),a); put(c.record,"detail",text(e.what(),a),a);
        c.npc.reset(); c.item.reset(); calls.Clear(); rows.Clear();
        }
      if(c.npc) put(c.record,"fields",fields(*c.npc,vm,a),a);
      if(c.item) put(c.record,"fields",fields(*c.item,vm,a),a);
      if(kind=="routines") {
        auto name=root->name(); auto split=name.find_last_of('_');
        put(c.record,"routine",text(name.substr(4,split-4),a),a);
        const auto number=name.substr(split+1);
        if(!number.empty() && number.find_first_not_of("0123456789")==std::string::npos)
          put(c.record,"npc_id",J(std::stoi(number)),a);
        else put(c.record,"npc_id",J(),a);
        put(c.record,"rows",std::move(rows),a);
        }
      put(c.record,"calls",std::move(calls),a);
      return c;
      }
  private:
    const NpcSnapshot::Program& program;
    Analysis analysis;
  };

std::map<std::string,std::vector<uint32_t>> corpus(const NpcSnapshot::Program& p) {
  auto vm=DaedalusScript::from_program(p.data);
  std::map<std::string,std::vector<uint32_t>> result{{"npcs",{}},{"items",{}},{"routines",{}},{"population",{}}};
  for(auto& [kind,cls]:std::map<std::string,std::string>{{"npcs","C_NPC"},{"items","C_ITEM"}})
    if(vm.find_symbol_by_name(cls)) vm.enumerate_instances_by_class_name(cls,[&](DaedalusSymbol& s) {
      if(s.is_const() && s.address()>0) result[kind].push_back(s.index());
      });
  for(auto& s:vm.symbols()) {
    if(s.type()!=Type::FUNCTION || s.is_external()) continue;
    if(s.name().starts_with("RTN_")) result["routines"].push_back(s.index());
    if(s.name().starts_with("STARTUP_") || s.name().starts_with("INIT_")) result["population"].push_back(s.index());
    }
  for(auto& [kind,ids]:result) {
    (void)kind;
    std::sort(ids.begin(),ids.end(),[&](uint32_t a,uint32_t b) { return p.data.symbols[a].name<p.data.symbols[b].name; });
    }
  return result;
  }
J envelope(const char* kind,const NpcSnapshot::Program& p,A& a) {
  J j(rapidjson::kObjectType);
  put(j,"format",text(std::string("gothic-npc-snapshot-")+kind,a),a); put(j,"version",J(1),a);
  put(j,"program",text(p.name,a),a); put(j,"sha256",text(p.sha256,a),a);
  J schema(rapidjson::kObjectType);
  put(schema,"INpc",text(INpcSchema,a),a); put(schema,"IItem",text(IItemSchema,a),a);
  put(j,"native_schema",std::move(schema),a);
  return j;
  }
void difference(const J& expected,const J& actual,const std::string& path,int& errors) {
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
    if(expected.Size()!=actual.Size()) { ++errors; std::cerr<<path<<": array size differs (expected "<<expected.Size()<<", read "<<actual.Size()<<")\n"; }
    for(rapidjson::SizeType i=0;i<std::min(expected.Size(),actual.Size());++i)
      difference(expected[i],actual[i],path+"["+std::to_string(i)+"]",errors);
    }
  else { ++errors; std::cerr<<path<<": value differs\n"; }
  }
}

NpcSnapshot::Program NpcSnapshot::load(const fs::path& base) {
  const auto index=ProgramData::readIndex(base/"programs");
  // Same default request and precedence as loadBaseProgram(GOTHIC.DAT, NONE).
  auto* source=index.findVfs("GOTHIC.DAT");
  if(source==nullptr) source=index.findHost("_work/Data/Scripts/_compiled/GOTHIC.DAT");
  require(source!=nullptr,"main program GOTHIC.DAT is absent from base index");
  Program p;
  p.name=source->program;
  p.data=ProgramData::readProgram(index.path(*source),&p.sha256);
  require(p.sha256==source->recordSha256,"main program SHA differs from base index");
  return p;
  }

namespace {
struct Summary {
  J counts{rapidjson::kObjectType},missing{rapidjson::kArrayType};
  std::map<std::string,size_t> population{{"WLD_INSERTNPC",0},{"WLD_INSERTITEM",0},{"WLD_ASSIGNROOMTOGUILD",0},{"WLD_SETMOBROUTINE",0}},deferred,reasons;
  size_t routineRows=0;
  void record(const std::string& kind,const J& record,A& a) {
    if(string(record["status"])!="captured") {
      J failure(rapidjson::kObjectType);
      put(failure,"kind",text(kind,a),a);
      for(const char* key:{"symbol","reason","detail"}) put(failure,key,copy(record[key],a),a);
      ++reasons[string(record["reason"])]; missing.PushBack(failure,a);
      }
    for(auto& call:record["calls"].GetArray()) {
      if(call.HasMember("deferred")) ++deferred[string(call["function"])];
      else if(kind=="population") ++population[string(call["function"])];
      }
    if(kind=="routines") routineRows+=record["rows"].Size();
    }
  void group(const std::string& kind,size_t total,size_t captured,A& a) {
    J count(rapidjson::kObjectType);
    put(count,"total",J(uint64_t(total)),a); put(count,"captured",J(uint64_t(captured)),a);
    put(count,"uncaptured",J(uint64_t(total-captured)),a); put(counts,kind.c_str(),std::move(count),a);
    }
  J finish(const NpcSnapshot::Program& p,A& a) {
    J report=envelope("report",p,a);
    put(counts,"routine_rows",J(uint64_t(routineRows)),a); put(report,"counts",std::move(counts),a);
    for(auto& [name,values]:std::map<std::string,std::map<std::string,size_t>>{{"population_calls",population},{"deferred_calls",deferred},{"uncaptured_reasons",reasons}}) {
      J j(rapidjson::kObjectType);
      for(auto& [key,n]:values) put(j,key.c_str(),J(uint64_t(n)),a);
      put(report,name.c_str(),std::move(j),a);
      }
    put(report,"uncaptured",std::move(missing),a);
    put(report,"string_encoding",text("Lossless Unicode U+0000..U+00FF mapping of script bytes",a),a);
    put(report,"language",text("One program only; NPC names and item texts depend on its language",a),a);
    return report;
    }
  };
}

void NpcSnapshot::write(const Program& p,const fs::path& output) {
  fs::create_directories(output);
  rapidjson::Document storage; auto& a=storage.GetAllocator();
  Runner runner(p);
  Summary summary;
  for(auto& [kind,ids]:corpus(p)) {
    J doc=envelope(kind.c_str(),p,a),records(rapidjson::kArrayType);
    size_t captured=0;
    for(auto id:ids) {
      auto c=runner.run(id,kind,a);
      if(string(c.record["status"])=="captured") ++captured;
      summary.record(kind,c.record,a);
      records.PushBack(c.record,a);
      }
    summary.group(kind,ids.size(),captured,a);
    put(doc,"records",std::move(records),a); writeJson(output/(kind+".json"),doc);
    }
  writeJson(output/"report.json",summary.finish(p,a));
  std::cout<<"NPC snapshot written: "<<output<<"\n";
  }

int NpcSnapshot::verify(const Program& p,const fs::path& input) {
  Runner runner(p);
  int errors=0;
  rapidjson::Document reportStorage;
  auto& reportAllocator=reportStorage.GetAllocator();
  Summary summary;
  for(auto& [kind,ids]:corpus(p)) {
    auto doc=readJson(input/(kind+".json")); auto& a=doc.GetAllocator();
    auto expected=envelope(kind.c_str(),p,a);
    require(doc.IsObject(),kind+": expected object");
    for(auto it=expected.MemberBegin();it!=expected.MemberEnd();++it)
      difference(it->value,at(doc,it->name.GetString()),kind+"."+it->name.GetString(),errors);
    auto& records=at(doc,"records"); require(records.IsArray(),kind+": expected records array");
    if(records.Size()!=ids.size()) { ++errors; std::cerr<<kind<<": record count differs\n"; }
    size_t captured=0;
    for(size_t i=0;i<ids.size();++i) {
      const auto path=kind+"."+p.data.symbols[ids[i]].name;
      auto c=runner.run(ids[i],kind,reportAllocator);
      if(string(c.record["status"])=="captured") ++captured;
      summary.record(kind,c.record,reportAllocator);
      if(i>=records.Size()) { ++errors; std::cerr<<path<<": missing record\n"; continue; }
      auto& actual=records[rapidjson::SizeType(i)];
      try {
        if(c.npc) verifyFields(*c.npc,at(actual,"fields"),*c.vm);
        if(c.item) verifyFields(*c.item,at(actual,"fields"),*c.vm);
        }
      catch(const std::exception& e) { ++errors; std::cerr<<path<<".fields."<<e.what()<<'\n'; }
      // Includes ordering of engine calls, deferred calls, rows, omissions and safe prefixes.
      difference(c.record,actual,path,errors);
      }
    summary.group(kind,ids.size(),captured,reportAllocator);
    }
  // Recompute report independently from the freshly verified records, without touching input.
  // Its provenance is mandatory even when the corpus is empty.
  auto report=readJson(input/"report.json");
  difference(summary.finish(p,reportAllocator),report,"report",errors);
  std::cout<<"NPC snapshot verification: "<<errors<<" differences\n";
  return errors;
  }

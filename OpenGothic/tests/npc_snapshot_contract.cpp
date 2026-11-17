#include "npcsnapshot.h"
#include "npc_definition_fixture.h"
#include "daedalus/classes.h"
#include "daedalus/vm.h"
#include "programdata.h"
#include "itemdefinitions.h"
#include "npcdefinitions.h"
#include "game/npcdefinitionruntime.h"
#include "game/itemdefinitionruntime.h"
#include "routinedefinitions.h"
#include "game/routinedefinitionruntime.h"
#include "world/objects/npcroutinetable.h"
#include "world/waypoint.h"
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <chrono>
#include <bit>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs=std::filesystem;
using namespace daedalus;
using Op=DaedalusOpcode;
using Type=DaedalusDataType;
namespace Flag=DaedalusSymbolFlag;
namespace {
int assertions=0;
void check(bool ok,const std::string& why) { ++assertions; if(!ok) throw std::runtime_error(why); }
struct Fixture {
  asset::ProgramRecord p;
  Fixture() { p.version=50; p.bytecode.push_back(std::byte{60}); }
  uint32_t symbol(std::string name,Type type,uint32_t count=1,uint32_t flags=0,int32_t parent=-1) {
    auto id=uint32_t(p.symbols.size());
    asset::ProgramSymbol s;
    s.name=std::move(name); s.type=uint32_t(type); s.count=count; s.flags=flags; s.parent=parent;
    if(!(flags&Flag::MEMBER)) {
      if(type==Type::INT) s.int_values.resize(count);
      if(type==Type::FLOAT) s.float_bits.resize(count);
      if(type==Type::STRING) s.string_values.resize(count);
      }
    p.symbols.push_back(std::move(s)); return id;
    }
  template<class T> uint32_t cls(const char* name) {
    auto id=symbol(name,Type::CLASS); p.symbols[id].class_size=sizeof(T);
    T t{};
    snapshotFields(t,[&](const char*,auto& value,bool function,const char* script) {
      using V=std::remove_all_extents_t<std::remove_reference_t<decltype(value)>>;
      auto type=std::is_same_v<V,std::string> ? Type::STRING : function ? Type::FUNCTION : Type::INT;
      auto count=std::is_array_v<std::remove_reference_t<decltype(value)>> ? uint32_t(std::extent_v<std::remove_reference_t<decltype(value)>>) : 1u;
      if(script[0]) symbol(script,type,count,Flag::MEMBER,int32_t(id));
      });
    return id;
    }
  uint32_t find(const char* name) const {
    for(uint32_t i=0;i<p.symbols.size();++i) if(p.symbols[i].name==name) return i;
    throw std::runtime_error(std::string("fixture missing ")+name);
    }
  uint32_t fn(const char* name,std::initializer_list<Type> params={},bool external=false,Type result=Type::VOID) {
    auto id=symbol(name,Type::FUNCTION,uint32_t(params.size()),Flag::CONST|(external?Flag::EXTERNAL:0)|(result!=Type::VOID?Flag::RETURN:0));
    p.symbols[id].return_type=uint32_t(result);
    int n=0;
    for(auto t:params) symbol(std::string(name)+".P"+std::to_string(n++),t);
    return id;
    }
  void begin(uint32_t id) { p.symbols[id].address=int32_t(p.bytecode.size()); }
  void op(Op op) { p.bytecode.push_back(std::byte(op)); }
  void op(Op op,uint32_t value) { this->op(op); for(int i=0;i<4;++i) p.bytecode.push_back(std::byte((value>>(8*i))&255)); }
  void set(uint32_t field,int value) { op(Op::PUSHI,uint32_t(value)); op(Op::PUSHV,field); op(Op::MOVI); }
  void call(uint32_t id) { op(Op::BL,uint32_t(p.symbols[id].address)); }
  NpcSnapshot::Program make() {
    auto npcClass=cls<INpc>("C_NPC"),itemClass=cls<IItem>("C_ITEM");
    auto self=symbol("SELF",Type::INSTANCE,1,0,int32_t(npcClass));
    auto global=symbol("CHAPTER",Type::INT);
    auto point=symbol("POINT",Type::STRING,1,Flag::CONST); p.symbols[point].string_values[0]="TEST_WP";
    auto random=fn("HLP_RANDOM",{Type::INT},true,Type::INT);
    auto visual=fn("MDL_SETVISUAL",{Type::INSTANCE,Type::STRING},true);
    auto fatness=fn("MDL_SETMODELFATNESS",{Type::INSTANCE,Type::FLOAT},true);
    auto equip=fn("EQUIPITEM",{Type::INSTANCE,Type::INT},true);
    auto ta=fn("TA_MIN",{Type::INSTANCE,Type::INT,Type::INT,Type::INT,Type::INT,Type::INT,Type::STRING},true);
    auto insert=fn("WLD_INSERTNPC",{Type::INT,Type::STRING},true);
    auto insertItem=fn("WLD_INSERTITEM",{Type::INT,Type::STRING},true);
    auto guild=fn("WLD_ASSIGNROOMTOGUILD",{Type::STRING,Type::INT},true);
    auto mob=fn("WLD_SETMOBROUTINE",{Type::INT,Type::INT,Type::STRING,Type::INT},true);
    auto state=fn("ZS_TEST"); begin(state); op(Op::RSR);
    auto randomLeaf=fn("RANDOM_LEAF"); begin(randomLeaf); op(Op::PUSHI,100); op(Op::BE,random); op(Op::RSR);
    auto ambient=fn("AMBIENT",{Type::INSTANCE}); begin(ambient);
    op(Op::PUSHV,ambient+1); op(Op::MOVVI); call(randomLeaf); op(Op::RSR);
    auto helper=fn("HELPER"); begin(helper); set(find("C_NPC.ATTRIBUTE"),42); op(Op::RSR);
    auto dependent=fn("DEPENDENT"); begin(dependent);
    op(Op::PUSHV,global); op(Op::PUSHV,find("C_NPC.LEVEL")); op(Op::MOVI); op(Op::RSR);
    auto prototype=symbol("NPC_BASE",Type::PROTOTYPE,0,Flag::CONST,int32_t(npcClass)); begin(prototype);
    set(find("C_NPC.ID"),123); call(helper); op(Op::RSR);
    auto item=symbol("IT_TEST",Type::INSTANCE,0,Flag::CONST,int32_t(itemClass)); begin(item);
    set(find("C_ITEM.VALUE"),9); set(find("C_ITEM.ON_EQUIP"),int(state)); op(Op::RSR);
    auto routine=fn("RTN_START_123"); begin(routine);
    for(int start:{8,20}) {
      op(Op::PUSHV,self); op(Op::PUSHI,uint32_t(start)); op(Op::PUSHI,5);
      op(Op::PUSHI,uint32_t(start==8?20:8)); op(Op::PUSHI,10); op(Op::PUSHI,state); op(Op::PUSHV,point); op(Op::BE,ta);
      }
    op(Op::RSR);
    auto npc=symbol("NPC_TEST",Type::INSTANCE,0,Flag::CONST,int32_t(prototype)); begin(npc);
    call(prototype); set(find("C_NPC.DAILY_ROUTINE"),int(routine));
    op(Op::PUSHV,self); op(Op::PUSHV,point); op(Op::BE,visual);
    op(Op::PUSHV,self); op(Op::PUSHI,item); op(Op::BE,equip);
    op(Op::PUSHV,self); op(Op::PUSHI,std::bit_cast<uint32_t>(1.2f)); op(Op::BE,fatness);
    op(Op::PUSHV,self); call(ambient); op(Op::RSR);
    auto bad=symbol("NPC_STATE",Type::INSTANCE,0,Flag::CONST,int32_t(prototype)); begin(bad); call(prototype); call(dependent); op(Op::RSR);
    auto startup=fn("STARTUP_TEST"); begin(startup);
    op(Op::PUSHI,npc); op(Op::PUSHV,point); op(Op::BE,insert);
    op(Op::PUSHI,item); op(Op::PUSHV,point); op(Op::BE,insertItem);
    op(Op::PUSHV,point); op(Op::PUSHI,3); op(Op::BE,guild);
    op(Op::PUSHI,8); op(Op::PUSHI,0); op(Op::PUSHV,point); op(Op::PUSHI,1); op(Op::BE,mob); op(Op::RSR);
    auto computed=fn("INIT_COMPUTED_STATE"); begin(computed);
    op(Op::PUSHI,npc); op(Op::PUSHV,point); op(Op::BE,insert);
    op(Op::PUSHV,global); op(Op::PUSHI,0); op(Op::EQ);
    op(Op::BZ,uint32_t(p.bytecode.size()+6)); op(Op::RSR); op(Op::RSR);
    auto init=fn("INIT_STATE"); begin(init);
    op(Op::PUSHI,npc); op(Op::PUSHV,point); op(Op::BE,insert);
    op(Op::PUSHV,global); op(Op::BZ,uint32_t(p.bytecode.size()+6)); op(Op::RSR); op(Op::RSR);
    auto encoded=ProgramData::encodeProgram(p);
    return {std::move(p),encoded.name,encoded.sha256};
    }
  };
rapidjson::Document read(const fs::path& path) {
  std::ifstream f(path); std::string s{std::istreambuf_iterator<char>(f),{}};
  rapidjson::Document j; j.Parse<rapidjson::kParseFullPrecisionFlag>(s.c_str()); check(!j.HasParseError(),"valid fixture JSON"); return j;
  }
void write(const fs::path& path,const rapidjson::Document& j) {
  rapidjson::StringBuffer bytes;
  rapidjson::Writer<rapidjson::StringBuffer> writer(bytes);
  check(j.Accept(writer),"encode fixture JSON");
  std::ofstream f(path,std::ios::binary);
  f.write(bytes.GetString(),std::streamsize(bytes.GetSize()));
  check(bool(f),"write fixture JSON");
  }
NpcSnapshot::Program itemProgram() {
  Fixture f;
  auto npcClass=f.cls<INpc>("C_NPC"),itemClass=f.cls<IItem>("C_ITEM");
  f.symbol("SELF",Type::INSTANCE,1,0,int32_t(npcClass));
  f.symbol("ITEM",Type::INSTANCE,1,0,int32_t(itemClass));
  auto chapter=f.symbol("CHAPTER",Type::INT);
  auto globalString=f.symbol("GLOBAL_STRING",Type::STRING);
  auto globalFloat=f.symbol("GLOBAL_FLOAT",Type::FLOAT);
  auto globalInstance=f.symbol("GLOBAL_INSTANCE",Type::INSTANCE,1,0,int32_t(npcClass));
  auto text=f.symbol("GLOBAL_TEXT",Type::STRING,1,Flag::CONST);
  f.p.symbols[text].string_values[0]="written by constructor";
  auto owner=f.symbol("ITEM_OWNER",Type::INSTANCE,0,Flag::CONST,int32_t(npcClass));
  f.begin(owner); f.op(Op::RSR);
  std::vector<uint32_t> callbacks;
  for(unsigned i=0;i<7;++i) {
    auto callback=f.fn(("ITEM_CALLBACK_"+std::to_string(i)).c_str());
    f.begin(callback); f.op(Op::RSR); callbacks.push_back(callback);
    }
  auto item=f.symbol("IT_RECORD_TEST",Type::INSTANCE,0,Flag::CONST,int32_t(itemClass));
  f.begin(item);
  unsigned number=100,callback=0;
  IItem fields{};
  snapshotFields(fields,[&](const char* name,auto& field,bool function,const char* binding) {
    const auto member=f.find(binding);
    auto assign=[&](auto& value,unsigned index) {
      using T=std::remove_reference_t<decltype(value)>;
      if constexpr(std::is_same_v<T,std::string>) {
        auto constant=f.symbol("TEXT_"+std::to_string(number++),Type::STRING,1,Flag::CONST);
        f.p.symbols[constant].string_values[0]=std::string(name)+std::to_string(index)+std::string("\0\x80\xff",3);
        f.op(Op::PUSHV,constant);
        }
      else {
        auto n=function ? (std::string_view(name)=="owner" ? owner : callbacks.at(callback++)) : number++;
        f.op(Op::PUSHI,n);
        }
      f.op(Op::PUSHVV,member); f.p.bytecode.push_back(std::byte(index));
      f.op(std::is_same_v<T,std::string> ? Op::MOVS : Op::MOVI);
      };
    if constexpr(std::is_array_v<std::remove_reference_t<decltype(field)>>) {
      for(unsigned i=0;i<std::extent_v<std::remove_reference_t<decltype(field)>>;++i) assign(field[i],i);
      }
    else assign(field,0);
    });
  f.op(Op::RSR);
  auto dependent=f.symbol("IT_RECORD_STATE",Type::INSTANCE,0,Flag::CONST,int32_t(itemClass));
  f.begin(dependent);
  f.op(Op::PUSHV,chapter); f.op(Op::PUSHV,f.find("C_ITEM.VALUE")); f.op(Op::MOVI); f.op(Op::RSR);
  auto writer=f.symbol("IT_RECORD_WRITE",Type::INSTANCE,0,Flag::CONST,int32_t(itemClass));
  f.begin(writer); f.set(chapter,7); f.op(Op::RSR);
  auto stringWriter=f.symbol("IT_RECORD_WRITE_STRING",Type::INSTANCE,0,Flag::CONST,int32_t(itemClass));
  f.begin(stringWriter);
  f.op(Op::PUSHV,text); f.op(Op::PUSHV,globalString); f.op(Op::MOVS); f.op(Op::RSR);
  auto floatWriter=f.symbol("IT_RECORD_WRITE_FLOAT",Type::INSTANCE,0,Flag::CONST,int32_t(itemClass));
  f.begin(floatWriter);
  f.op(Op::PUSHI,std::bit_cast<uint32_t>(1.25f)); f.op(Op::PUSHV,globalFloat); f.op(Op::MOVF); f.op(Op::RSR);
  auto instanceWriter=f.symbol("IT_RECORD_WRITE_INSTANCE",Type::INSTANCE,0,Flag::CONST,int32_t(itemClass));
  f.begin(instanceWriter);
  f.op(Op::PUSHV,owner); f.op(Op::PUSHV,globalInstance); f.op(Op::MOVVI); f.op(Op::RSR);
  for(int value:{0,-1}) {
    auto sentinel=f.symbol(value==0 ? "IT_RECORD_NULL" : "IT_RECORD_MINUS1",Type::INSTANCE,0,Flag::CONST,int32_t(itemClass));
    f.begin(sentinel);
    for(const char* field:{"C_ITEM.MAGIC","C_ITEM.ON_EQUIP","C_ITEM.ON_UNEQUIP","C_ITEM.OWNER"})
      f.set(f.find(field),value);
    for(unsigned i=0;i<ItemRecord::state_count;++i) {
      f.op(Op::PUSHI,uint32_t(value)); f.op(Op::PUSHVV,f.find("C_ITEM.ON_STATE"));
      f.p.bytecode.push_back(std::byte(i)); f.op(Op::MOVI);
      }
    f.op(Op::RSR);
    }
  auto encoded=ProgramData::encodeProgram(f.p);
  return {std::move(f.p),encoded.name,encoded.sha256};
  }
NpcSnapshot::Program routineProgram() {
  Fixture f;
  const auto npcClass=f.cls<INpc>("C_NPC");
  const auto self=f.symbol("SELF",Type::INSTANCE,1,0,int32_t(npcClass));
  const auto global=f.symbol("CHAPTER",Type::INT);
  const auto known=f.symbol("KNOWN_POINT",Type::STRING,1,Flag::CONST);
  const auto unknown=f.symbol("UNKNOWN_POINT",Type::STRING,1,Flag::CONST);
  f.p.symbols[known].string_values[0]="TEST_WP";
  f.p.symbols[unknown].string_values[0]=std::string("UNKNOWN_WP\x80\xff",12);
  const auto ta=f.fn("TA_MIN",{Type::INSTANCE,Type::INT,Type::INT,Type::INT,Type::INT,Type::INT,Type::STRING},true);
  const auto state=f.fn("ZS_ROUTINE"); f.begin(state); f.op(Op::RSR);
  const auto wrapper=f.fn("TA_RECORD",{Type::INSTANCE,Type::INT,Type::INT,Type::INT,Type::INT,Type::STRING});
  f.begin(wrapper);
  for(int parameter=6;parameter>0;--parameter) {
    f.op(Op::PUSHV,wrapper+uint32_t(parameter));
    f.op(parameter==6 ? Op::MOVS : parameter==1 ? Op::MOVVI : Op::MOVI);
    }
  for(unsigned parameter=1;parameter<=5;++parameter) f.op(Op::PUSHV,wrapper+parameter);
  f.op(Op::PUSHI,state); f.op(Op::PUSHV,wrapper+6); f.op(Op::BE,ta); f.op(Op::RSR);
  const auto routine=f.fn("RTN_START_123"); f.begin(routine);
  // Adjacent pairs share starts; interleaved starts distinguish std::sort from stable_sort.
  // End minutes identify input order independently of either table-building path.
  for(unsigned row=0;row<48;++row) {
    f.op(Op::PUSHV,self); f.op(Op::PUSHI,row%4<2 ? 20 : 8); f.op(Op::PUSHI,5);
    f.op(Op::PUSHI,23); f.op(Op::PUSHI,row); f.op(Op::PUSHV,row%3==0 ? unknown : known);
    f.call(wrapper);
    }
  f.op(Op::RSR);
  const auto read=f.fn("RTN_READ_123");
  const auto local=f.symbol("RTN_READ_123.VALUE",Type::INT);
  f.begin(read); f.op(Op::PUSHV,global); f.op(Op::PUSHV,local); f.op(Op::MOVI); f.op(Op::RSR);
  const auto write=f.fn("RTN_WRITE_123"); f.begin(write); f.set(global,9); f.op(Op::RSR);
  const auto helper=f.fn("OTHER_HELPER"); f.begin(helper); f.op(Op::RSR);
  const auto unsupported=f.fn("RTN_HELPER_123"); f.begin(unsupported); f.call(helper); f.op(Op::RSR);
  const auto encoded=ProgramData::encodeProgram(f.p);
  return {std::move(f.p),encoded.name,encoded.sha256};
  }

void routineDefinitions(const fs::path& dir) {
  const auto p=routineProgram();
  const auto base=dir/"routine-definitions";
  const auto file=base/"definitions"/p.sha256/"routines.json";
  const auto counts=NpcSnapshot::writeRoutines(p,file);
  check(counts.total==4 && counts.captured==1,"only the pure routine is captured");
  check(NpcSnapshot::verifyRoutines(p,file)==0,"routine definitions verify from disk");
  const auto library=RoutineDefinitions::read(base,p.sha256);
  for(const char* name:{"RTN_READ_123","RTN_WRITE_123","RTN_HELPER_123"})
    check(library.uncaptured.contains(name),std::string(name)+": unsafe routine retains its refusal");
  const auto& source=library.routines.at("RTN_START_123");
  check(source.size()==48,"all routine calls retain their order in the section");
  for(size_t row=0;row<source.size();++row)
    check(source[row].endMinute==int(row),"section rows follow TA_Min call order");

  WayPoint known;
  known.name="TEST_WP";
  std::vector<NpcRoutineTable::Row> actual,expected;
  auto append=[&](auto& table,int sh,int sm,int eh,int em,uint32_t state,std::string_view point) {
    NpcRoutineTable::add(table,gtime(sh,sm),gtime(eh,em),state,point==known.name ? &known : nullptr,point);
    };
  auto script=DaedalusScript::from_program(p.data);
  INpc::register_(script);
  auto definitions=RoutineDefinitionRuntime::load(base,script,p.sha256);
  DaedalusVm vm(std::move(script));
  auto self=vm.allocate_instance<INpc>(vm.global_self());
  vm.unsafe_set_gi(self);
  vm.push_int(0x12345678);
  const auto pc=vm.pc();
  vm.register_external("TA_MIN",[&](std::shared_ptr<INpc> npc,int sh,int sm,int eh,int em,int state,std::string_view point) {
    check(npc==self,"VM routine rows belong to the same NPC");
    append(expected,sh,sm,eh,em,uint32_t(state),point);
    });
  auto* function=vm.find_symbol_by_name("RTN_START_123");
  size_t callIndex=0;
  RoutineDefinitionRuntime::initialize(vm,function,definitions,[&](const RoutineDefinitionRuntime::Row& row) {
    check(row.endMinute==int(callIndex++),"record fill follows TA_Min input order before sorting");
    append(actual,row.startHour,row.startMinute,row.endHour,row.endMinute,row.state,row.waypoint);
    });
  check(expected.empty(),"captured routine does not execute TA_Min in the VM");
  vm.call_function<void>(function);
  check(actual.size()==48 && actual.size()==expected.size(),"record and VM tables have every row");
  for(size_t index=0;index<actual.size();++index) {
    const auto& row=actual[index];
    const auto& original=expected[index];
    const auto call=unsigned(index<24 ? (index/2)*4+2+index%2 : ((index-24)/2)*4+(index-24)%2);
    check(row.start==gtime(call%4<2 ? 20 : 8,5) && row.end==gtime(23,int(call)),
          "tied rows retain their original TA_Min call order");
    check(row.start==original.start && row.end==original.end && row.callback==original.callback &&
          row.point==original.point && row.fallbackName==original.fallbackName && row.wayPointName()==original.wayPointName(),
          "record and VM tables have identical times, state, point identity and fallback name");
    check(row.point==(call%3==0 ? nullptr : &known),"known points resolve to their original pointer");
    check(row.fallbackName==(call%3==0 ? std::string("UNKNOWN_WP\x80\xff",12) : ""),
          "unknown points retain every fallback byte");
    }
  check(vm.global_self()->get_instance()==self && vm.unsafe_get_gi()==self && vm.pc()==pc,
        "routine paths preserve SELF, VM context and program counter");
  check(vm.pop_int()==0x12345678,"routine paths preserve the caller's stack");
  RoutineDefinitionRuntime::initialize(vm,vm.find_symbol_by_name("RTN_WRITE_123"),definitions,
                                       [](const RoutineDefinitionRuntime::Row&) {});
  check(vm.find_symbol_by_name("CHAPTER")->get_int()==9,"uncaptured routine keeps its VM side effect");

  auto changed=read(file);
  for(auto& record:changed["records"].GetArray())
    if(std::string_view(record["symbol"].GetString())=="RTN_START_123")
      record["rows"][0]["start"][1].SetInt(6);
  write(file,changed);
  check(NpcSnapshot::verifyRoutines(p,file)>0,"routine verification rejects a changed time with its row");
  NpcSnapshot::writeRoutines(p,file);
  const auto otherFile=dir/"routines-again.json";
  NpcSnapshot::writeRoutines(p,otherFile);
  std::ifstream first(file,std::ios::binary),second(otherFile,std::ios::binary);
  check(std::string(std::istreambuf_iterator<char>(first),{})==std::string(std::istreambuf_iterator<char>(second),{}),
        "routine sections are byte-identical across repeated captures");
  std::cout<<"Routine runtime equality: 48 rows, adjacent equal starts, 24 tied rows per start, known and unknown points; VM state preserved\n";
  }

void routineSelection(const fs::path& dir) {
  const auto base=dir/"routine-selection";
  auto selected=routineProgram(),other=selected;
  for(auto& symbol:other.data.symbols)
    if(symbol.name=="KNOWN_POINT") symbol.string_values[0]="OTHER_WP";
  const auto encoded=ProgramData::encodeProgram(other.data);
  other.name=encoded.name; other.sha256=encoded.sha256;
  for(const auto* p:{&selected,&other})
    NpcSnapshot::writeRoutines(*p,base/"definitions"/p->sha256/"routines.json");
  auto program=DaedalusScript::from_program(selected.data);
  auto another=DaedalusScript::from_program(other.data);
  const auto symbol=program.find_symbol_by_name("RTN_START_123")->index();
  const char* previous=std::getenv("OPENGOTHIC_ROUTINE_RECORDS");
  const bool hadPrevious=previous!=nullptr;
  const std::string saved=previous ? previous : "";
  auto environment=[](const char* value) {
#ifdef _WIN32
    check(_putenv_s("OPENGOTHIC_ROUTINE_RECORDS",value ? value : "")==0,"set routine records environment");
#else
    check((value ? setenv("OPENGOTHIC_ROUTINE_RECORDS",value,1) : unsetenv("OPENGOTHIC_ROUTINE_RECORDS"))==0,
          "set routine records environment");
#endif
    };
  try {
    environment(nullptr);
    check(RoutineDefinitionRuntime::load(base,program,selected.sha256).routines.at(symbol)[1].waypoint=="TEST_WP",
          "routine records are enabled by default and use the loaded program section");
    environment("0");
    check(RoutineDefinitionRuntime::load(base/"absent",program,selected.sha256).routines.empty(),
          "zero disables routine records without reading the base");
    environment("1");
    check(RoutineDefinitionRuntime::load(base,program,selected.sha256).routines.contains(symbol),
          "nonzero enables routine records");
    check(RoutineDefinitionRuntime::load(base,another,other.sha256).routines.at(symbol)[1].waypoint=="OTHER_WP",
          "changing the loaded program changes the routine section");
    }
  catch(...) { environment(hadPrevious ? saved.c_str() : nullptr); throw; }
  environment(hadPrevious ? saved.c_str() : nullptr);
  }
template<class T> bool sameValue(const T& a,const T& b) {
  if constexpr(std::is_array_v<T>) {
    for(size_t i=0;i<std::extent_v<T>;++i) if(!sameValue(a[i],b[i])) return false;
    return true;
    }
  else return a==b;
  }
void sameItem(IItem& expected,IItem& actual,const std::string& symbol) {
  snapshotFields(expected,[&](const char* name,auto& value,bool,const char*) {
    bool equal=false;
    snapshotFields(actual,[&](const char* other,auto& result,bool,const char*) {
      if constexpr(std::is_same_v<std::remove_reference_t<decltype(value)>,std::remove_reference_t<decltype(result)>>)
        if(std::string_view(name)==other) equal=sameValue(value,result);
      });
    check(equal,symbol+"."+name+": runtime definition differs from constructor");
    });
  }
std::unique_ptr<DaedalusVm> itemVm(const NpcSnapshot::Program& p) {
  auto script=DaedalusScript::from_program(p.data);
  IItem::register_(script);
  return std::make_unique<DaedalusVm>(std::move(script));
  }
void runtimeItems(const NpcSnapshot::Program& p,const fs::path& base) {
  auto library=ItemDefinitions::read(base,p.sha256);
  auto script=DaedalusScript::from_program(p.data);
  auto definitions=ItemDefinitionRuntime::load(base,script,p.sha256);
  IItem::register_(script);
  auto records=std::make_unique<DaedalusVm>(std::move(script));
  size_t captured=0,fallback=0;
  for(auto& symbol:records->symbols()) {
    if(symbol.type()!=Type::INSTANCE || !symbol.is_const()) continue;
    auto* parent=records->find_symbol_by_index(symbol.parent());
    while(parent && parent->type()!=Type::CLASS) parent=records->find_symbol_by_index(parent->parent());
    if(!parent || parent->name()!="C_ITEM") continue;
    auto original=itemVm(p);
    auto self=std::make_shared<INpc>();
    auto previousItem=std::make_shared<IItem>();
    auto prepare=[&](DaedalusVm& vm) {
      if(vm.global_self()) vm.global_self()->set_instance(self);
      if(vm.global_item()) vm.global_item()->set_instance(previousItem);
      vm.unsafe_set_gi(self);
      vm.push_int(0x12345678);
      };
    prepare(*records); prepare(*original);
    const auto recordPc=records->pc(),originalPc=original->pc();
    auto actual=std::make_shared<IItem>();
    actual->user_ptr=&fallback;
    auto* boundSymbol=records->find_symbol_by_index(symbol.index());
    const bool usedRecord=ItemDefinitionRuntime::initialize(*records,actual,boundSymbol,definitions);
    auto expected=original->init_instance<IItem>(original->find_symbol_by_index(symbol.index()));
    sameItem(*expected,*actual,symbol.name());
    check(usedRecord==library.items.contains(symbol.name()),symbol.name()+": captured item uses runtime definition");
    check(actual->symbol_index()==expected->symbol_index(),symbol.name()+": symbol binding is preserved");
    check(boundSymbol->get_instance()==actual,symbol.name()+": VM symbol points to the new item");
    check(actual->user_ptr==&fallback,symbol.name()+": native owner is preserved");
    auto state=[&](DaedalusVm& vm,uint32_t pc) {
      check(!vm.global_self() || vm.global_self()->get_instance()==self,symbol.name()+": SELF is restored");
      check(!vm.global_item() || vm.global_item()->get_instance()==previousItem,symbol.name()+": ITEM is preserved");
      check(vm.unsafe_get_gi()==self,symbol.name()+": VM context is restored");
      check(vm.pc()==pc,symbol.name()+": VM program counter is restored");
      check(vm.pop_int()==0x12345678,symbol.name()+": VM stack is preserved");
      };
    state(*records,recordPc); state(*original,originalPc);
    if(usedRecord) ++captured; else ++fallback;
    }
  check(captured==library.items.size(),"every captured definition was compared in the runtime");
  check(fallback==library.uncaptured.size(),"every uncaptured item used its constructor");
  std::cout<<"Item runtime equality: "<<captured<<" captured, "<<fallback<<" VM fallback; every ItemRecord field and VM state match\n";
  }
void npcDefinitions(const fs::path& dir) {
  const auto program=NpcDefinitionFixture::program();
  const auto base=dir/"npc-definitions",file=base/"definitions"/program.sha256/"npcs.json";
  const auto counts=NpcSnapshot::writeNpcs(program,file);
  check(counts.total==8,"all eight fixture NPC definitions are imported");
  check(NpcSnapshot::verifyNpcs(program,file)==0,"NPC definitions verify independently from disk");
  const auto library=NpcDefinitions::read(base,program.sha256);
  for(const auto* refused:{"NPC_GLOBAL","NPC_WRITER","NPC_UNKNOWN"})
    check(library.uncaptured.contains(refused),std::string(refused)+": refuses unsafe constructor");
  check(library.uncaptured.contains("NPC_BAD_ITEM"),"NPC_BAD_ITEM: invalid item prototype retains its VM script error");
  check(library.uncaptured.contains("NPC_ENGINE_READ") &&
        library.uncaptured.at("NPC_ENGINE_READ").starts_with("engine_state_read: C_NPC.ATTRIBUTE[0]"),
        "a constructor-body read after equipment has no safe helper boundary");
  check(library.uncaptured.contains("NPC_INITIAL_READ") &&
        library.uncaptured.at("NPC_INITIAL_READ")=="engine_state_read: C_NPC.ID[0] before assignment",
        "a constructor reading the engine's initial field retains the VM fallback");
  check(counts.captured==2 && library.uncaptured.size()==6,"two fixture NPCs are captured and six retain the VM fallback");
  const auto& steps=library.npcs.at("NPC_NO_ID").steps;
  check(std::any_of(steps.begin(),steps.end(),[](const auto& step) {
    const auto* call=std::get_if<NpcDefinitions::Call>(&step);
    return call && call->function=="ADD_AFTER_EQUIP" && call->deferred;
    }),"a helper reading an engine-modified field is deferred independently of its name");
  auto invalid=read(file);
  for(auto& record:invalid["records"].GetArray()) {
    if(std::string_view(record["symbol"].GetString())!="NPC_BAD_ITEM") continue;
    check(std::string_view(record["reason"].GetString())=="script_error","item allocation errors use the generic script_error reason");
    check(std::string_view(record["detail"].GetString())=="Cannot init IT_NPC_PROTOTYPE: not an instance",
          "item allocation preserves the original VM exception message");
    record["status"].SetString("captured",invalid.GetAllocator());
    record.RemoveMember("reason"); record.RemoveMember("detail");
    }
  write(file,invalid);
  check(NpcSnapshot::verifyNpcs(program,file)>0,"verification independently rejects a captured NPC with an invalid item prototype");
  NpcSnapshot::writeNpcs(program,file);
  const auto second=dir/"npc-identical.json";
  NpcSnapshot::writeNpcs(program,second);
  std::ifstream a(file,std::ios::binary),b(second,std::ios::binary);
  check(std::string(std::istreambuf_iterator<char>(a),{})==std::string(std::istreambuf_iterator<char>(b),{}),
        "NPC definitions are byte-identical across fresh imports");
  {
    auto script=DaedalusScript::from_program(program.data);
    INpc::register_(script); IItem::register_(script);
    DaedalusVm vm(std::move(script));
    const auto definitions=NpcDefinitionRuntime::resolve(library,vm);
    auto npc=std::make_shared<INpc>();
    check(!NpcDefinitionRuntime::initialize(vm,npc,vm.find_symbol_by_name("NPC_WRITER"),definitions),
          "uncaptured NPC uses the same VM fallback as the game");
    check(vm.find_symbol_by_name("CHAPTER")->get_int()==9,"fallback preserves constructor side effects");
    }
  auto changed=read(file);
  for(auto& record:changed["records"].GetArray()) {
    if(std::string_view(record["symbol"].GetString())!="NPC_NO_ID") continue;
    for(auto& step:record["steps"].GetArray())
      if(step.HasMember("field") && std::string_view(step["field"].GetString())=="C_NPC.ATTRIBUTE" &&
         step["value"]["value"].GetInt()==10) step["value"]["value"].SetInt(999);
    }
  write(file,changed);
  check(NpcSnapshot::verifyNpcs(program,file)>0,"fresh import verification rejects a changed NPC field on disk");
  NpcSnapshot::writeNpcs(program,file);

  auto instruction=[&](NpcSnapshot::Program& value,uint32_t pc,Op op,uint32_t operand) {
    check(size_t(pc)+5<=value.data.bytecode.size(),"NPC fixture instruction is within bytecode");
    value.data.bytecode[pc]=std::byte(op);
    for(uint32_t i=0;i<4;++i) value.data.bytecode[pc+1+i]=std::byte((operand>>(8*i))&255);
    };
  auto encode=[](NpcSnapshot::Program& value) {
    const auto encoded=ProgramData::encodeProgram(value.data);
    value.name=encoded.name; value.sha256=encoded.sha256;
    };
  {
    auto randomGlobal=program;
    auto script=DaedalusScript::from_program(program.data);
    const auto* helper=script.find_symbol_by_name("B_CREATEAMBIENTINV");
    const auto* random=script.find_symbol_by_name("HLP_RANDOM");
    const auto* global=script.find_symbol_by_name("CHAPTER");
    bool changedBound=false;
    for(auto pc=helper->address();;) {
      const auto op=script.instruction_at(pc);
      if(op.op==Op::RSR) break;
      if(op.op==Op::PUSHI && op.immediate==7) {
        const auto next=script.instruction_at(pc+op.size);
        if(next.op==Op::BE && next.symbol==random->index()) {
          instruction(randomGlobal,pc,Op::PUSHV,global->index());
          changedBound=true; break;
          }
        }
      pc+=op.size;
      }
    check(changedBound,"random helper fixture contains the expected random bound");
    encode(randomGlobal);
    const auto unsafeBase=dir/"npc-random-global";
    NpcSnapshot::writeNpcs(randomGlobal,unsafeBase/"definitions"/randomGlobal.sha256/"npcs.json");
    const auto unsafe=NpcDefinitions::read(unsafeBase,randomGlobal.sha256);
    check(unsafe.uncaptured.contains("NPC_NO_ID") && unsafe.uncaptured.at("NPC_NO_ID")=="game_state: CHAPTER",
          "a deferred random helper must not hide a mutable global read");
    check(unsafe.npcs.contains("NPC_WITH_ID"),"an unreachable unsafe random helper does not reject another NPC");
    }
  {
    auto malformed=program;
    auto script=DaedalusScript::from_program(program.data);
    auto pc=script.find_symbol_by_name("NPC_WITH_ID")->address();
    pc+=script.instruction_at(pc).size; // The inherited constructor remains valid.
    const auto field=pc+script.instruction_at(pc).size;
    check(script.instruction_at(pc).op==Op::PUSHI && script.instruction_at(field).op==Op::PUSHV,
          "NPC fixture contains its direct field assignment");
    instruction(malformed,pc,Op::PUSHI,0x7fffffff);
    instruction(malformed,field,Op::PUSHV,script.find_symbol_by_name("C_NPC.DAILY_ROUTINE")->index());
    encode(malformed);
    const auto broken=dir/"npc-invalid-reference.json";
    std::string error;
    try { NpcSnapshot::writeNpcs(malformed,broken); }
    catch(const std::runtime_error& failure) { error=failure.what(); }
    check(error=="NPC definitions: NPC_WITH_ID: invalid symbol reference 2147483647",
          "a snapshot invariant aborts import with the NPC name instead of recording execution_error");
    check(!fs::exists(broken),"an invariant failure does not publish a partial NPC section");
    }

  const auto* setting=std::getenv("OPENGOTHIC_NPC_RECORDS");
  const bool present=setting!=nullptr;
  const std::string previous=setting ? setting : "";
  auto environment=[](const char* value) {
#ifdef _WIN32
    check(_putenv_s("OPENGOTHIC_NPC_RECORDS",value ? value : "")==0,"set NPC records environment");
#else
    check((value ? setenv("OPENGOTHIC_NPC_RECORDS",value,1) : unsetenv("OPENGOTHIC_NPC_RECORDS"))==0,"set NPC records environment");
#endif
    };
  try {
    auto script=DaedalusScript::from_program(program.data);
    environment(nullptr);
    check(NpcDefinitionRuntime::enabled() && NpcDefinitionRuntime::load(base,script,program.sha256).npcs.size()==2,
          "NPC records are enabled by default through the game's shared switch");
    environment("0");
    check(!NpcDefinitionRuntime::enabled() && NpcDefinitionRuntime::load(base/"absent",script,program.sha256).npcs.empty(),
          "zero disables NPC records without reading the base");
    environment("1");
    check(NpcDefinitionRuntime::enabled() && NpcDefinitionRuntime::load(base,script,program.sha256).npcs.size()==2,
          "nonzero enables NPC records");
    }
  catch(...) { environment(present ? previous.c_str() : nullptr); throw; }
  environment(present ? previous.c_str() : nullptr);
  }

void itemDefinitions(const fs::path& dir) {
  auto p=itemProgram();
  const auto base=dir/"definitions-fixture";
  const auto file=base/"definitions"/p.sha256/"items.json";
  NpcSnapshot::writeItems(p,file);
  check(NpcSnapshot::verifyItems(p,file)==0,"item definitions verify from disk");
  auto snapshot=read(file);
  for(const auto& [name,global]:std::initializer_list<std::pair<const char*,const char*>>{
        {"IT_RECORD_WRITE","CHAPTER"},{"IT_RECORD_WRITE_STRING","GLOBAL_STRING"},
        {"IT_RECORD_WRITE_FLOAT","GLOBAL_FLOAT"},{"IT_RECORD_WRITE_INSTANCE","GLOBAL_INSTANCE"}}) {
    bool foundWriter=false;
    for(const auto& record:snapshot["records"].GetArray()) {
      if(std::string_view(record["symbol"].GetString())!=name) continue;
      foundWriter=true;
      check(std::string_view(record["status"].GetString())=="uncaptured",std::string(name)+": global-writing item is not captured");
      check(std::string_view(record["reason"].GetString())=="game_state_write",std::string(name)+": names the refusal reason");
      check(std::string_view(record["detail"].GetString())==global,std::string(name)+": names the changed variable");
      check(!record.HasMember("fields"),std::string(name)+": has no captured fields");
      }
    check(foundWriter,std::string(name)+": global-writing item remains in definitions");
    }
  const auto library=ItemDefinitions::read(base,p.sha256);
  auto vm=itemVm(p);
  auto definitions=ItemDefinitionRuntime::resolve(library,*vm);
  check(!ItemDefinitionRuntime::initialize(*vm,std::make_shared<IItem>(),vm->find_symbol_by_name("IT_RECORD_WRITE"),definitions),
        "global-writing item uses the VM constructor");
  check(vm->find_symbol_by_name("CHAPTER")->get_int()==7,"VM fallback preserves the constructor's global write");
  for(int value:{0,-1}) {
    const std::string name=value==0 ? "IT_RECORD_NULL" : "IT_RECORD_MINUS1";
    const auto& definition=library.items.at(name);
    const auto& record=definition.record;
    check(record.magic==value && record.on_equip==value && record.on_unequip==value && record.owner==value,
          name+": function and owner sentinels decode to "+std::to_string(value));
    for(auto state:record.on_state) check(state==value,name+".on_state: sentinel is preserved");
    for(const auto& [field,names]:definition.symbols)
      for(const auto& symbol:names) check(symbol.empty(),name+"."+field+": sentinel has no symbol name to resolve");
    }
  runtimeItems(p,base);
  auto changed=read(file);
  for(auto& record:changed["records"].GetArray()) {
    if(std::string_view(record["symbol"].GetString())!="IT_RECORD_NULL") continue;
    auto& function=record["fields"]["on_equip"];
    check(function.IsNull(),"zero function sentinel is written as JSON null");
    function.SetString("",changed.GetAllocator());
    write(file,changed);
    bool rejected=false;
    try { ItemDefinitions::read(base,p.sha256); }
    catch(const std::runtime_error& e) {
      rejected=std::string(e.what()).find("IT_RECORD_NULL: on_equip: empty symbol name")!=std::string::npos;
      }
    check(rejected,"empty JSON function name is rejected with its item and field");
    function.SetNull();
    }
  for(auto& record:changed["records"].GetArray())
    if(record.HasMember("fields")) record["fields"]["value"].SetInt(1234567);
  write(file,changed);
  bool rejected=false;
  try { runtimeItems(p,base); }
  catch(const std::runtime_error& e) {
    rejected=std::string(e.what()).find("IT_RECORD_TEST.value")!=std::string::npos;
    }
  check(rejected,"runtime equality rejects a changed item number with its symbol and field");
  }
void npcLayout(const NpcSnapshot::Program& program) {
  auto script=DaedalusScript::from_program(program.data);
  INpc::register_(script); IItem::register_(script);
  DaedalusVm vm(std::move(script));
  auto npc=vm.allocate_instance<INpc>("NPC_TEST");
  auto item=vm.allocate_instance<IItem>("IT_TEST");
  auto& record=static_cast<NpcRecord&>(*npc);
  const auto begin=reinterpret_cast<uintptr_t>(npc.get());
  const auto recordBegin=reinterpret_cast<uintptr_t>(&record);
  check(recordBegin>=begin && recordBegin+sizeof(record)<=begin+sizeof(*npc),"whole NpcRecord lies within INpc");
  check(reinterpret_cast<uintptr_t>(static_cast<DaedalusInstance*>(npc.get()))==begin,"VM data starts at INpc");
  std::string signature;
  unsigned fields=0;
  snapshotFields(*npc,[&](const char* name,auto& field,bool,const char* binding) {
    ++fields;
    signature+=std::string(name)+":"+binding+" ";
    auto* symbol=vm.find_symbol_by_name(binding);
    check(symbol!=nullptr,std::string("bound NPC field ")+name);
    check(symbol->registered_to()==typeid(INpc),std::string("INpc context for ")+name);
    const auto address=reinterpret_cast<uintptr_t>(&field);
    check(symbol->offset_as_member()==address-begin,std::string("VM offset for ")+name);
    check(address>=recordBegin && address+sizeof(field)<=recordBegin+sizeof(record),std::string("record owns ")+name);
    auto value=[&](auto& element,uint16_t index) {
      using T=std::remove_reference_t<decltype(element)>;
      if constexpr(std::is_same_v<T,std::string>) {
        symbol->set_string("from VM",index,npc.get());
        check(element=="from VM",std::string("VM writes record ")+name);
        element="from record";
        check(symbol->get_string(index,npc.get())=="from record",std::string("VM reads record ")+name);
        }
      else {
        symbol->set_int(1000+index,index,npc.get());
        check(int32_t(element)==1000+index,std::string("VM writes record ")+name);
        element=T(2000+index);
        check(symbol->get_int(index,npc.get())==2000+index,std::string("VM reads record ")+name);
        }
      };
    if constexpr(std::is_array_v<std::remove_reference_t<decltype(field)>>) {
      for(uint16_t i=0;i<std::extent_v<std::remove_reference_t<decltype(field)>>;++i) value(field[i],i);
      }
    else value(field,0);
    });
  check(fields==32,"all 32 NPC fields checked");
  // Independent of the generated visitor: changing declaration order or a binding must fail.
  check(signature==
    "id:C_NPC.ID name:C_NPC.NAME slot:C_NPC.SLOT effect:C_NPC.EFFECT type:C_NPC.NPCTYPE flags:C_NPC.FLAGS "
    "attribute:C_NPC.ATTRIBUTE hitchance:C_NPC.HITCHANCE protection:C_NPC.PROTECTION damage:C_NPC.DAMAGE "
    "damage_type:C_NPC.DAMAGETYPE guild:C_NPC.GUILD level:C_NPC.LEVEL mission:C_NPC.MISSION "
    "fight_tactic:C_NPC.FIGHT_TACTIC weapon:C_NPC.WEAPON voice:C_NPC.VOICE voice_pitch:C_NPC.VOICEPITCH "
    "body_mass:C_NPC.BODYMASS daily_routine:C_NPC.DAILY_ROUTINE start_aistate:C_NPC.START_AISTATE "
    "spawnpoint:C_NPC.SPAWNPOINT spawn_delay:C_NPC.SPAWNDELAY senses:C_NPC.SENSES senses_range:C_NPC.SENSES_RANGE "
    "aivar:C_NPC.AIVAR wp:C_NPC.WP exp:C_NPC.EXP exp_next:C_NPC.EXP_NEXT lp:C_NPC.LP "
    "bodystate_interruptable_override:C_NPC.BODYSTATEINTERRUPTABLEOVERRIDE no_focus:C_NPC.NOFOCUS ",
    "original NPC field order and bindings");
  check(std::extent_v<decltype(record.name)> == 5,"original NPC name array size");
  check(std::extent_v<decltype(record.attribute)> == 8,"original NPC attribute array size");
  check(std::extent_v<decltype(record.hitchance)> == 5,"original NPC hitchance array size");
  check(std::extent_v<decltype(record.protection)> == 8,"original NPC protection array size");
  check(std::extent_v<decltype(record.damage)> == 8,"original NPC damage array size");
  check(std::extent_v<decltype(record.mission)> == 5,"original NPC mission array size");
  check(std::extent_v<decltype(record.aivar)> == 100,"original NPC aivar array size");
  bool rejected=false;
  try { vm.find_symbol_by_name("C_NPC.ID")->set_int(1,0,item.get()); }
  catch(const DaedalusIllegalContextType&) { rejected=true; }
  check(rejected,"NPC member rejects an item context");
  std::cout<<"NPC layout: "<<fields<<" fields, sizeof(INpc)="<<sizeof(INpc)
           <<", sizeof(NpcRecord)="<<sizeof(NpcRecord)<<", record offset="<<recordBegin-begin<<'\n';
  }
template<class F> void focusFields(IFocus& v,F f) {
#define FOCUS_FIELD(name,script) f(#name,v.name,false,"C_FOCUS." script)
  FOCUS_FIELD(npc_longrange,"NPC_LONGRANGE");
  FOCUS_FIELD(npc_range1,"NPC_RANGE1"); FOCUS_FIELD(npc_range2,"NPC_RANGE2");
  FOCUS_FIELD(npc_azi,"NPC_AZI"); FOCUS_FIELD(npc_elevdo,"NPC_ELEVDO"); FOCUS_FIELD(npc_elevup,"NPC_ELEVUP");
  FOCUS_FIELD(npc_prio,"NPC_PRIO");
  FOCUS_FIELD(item_range1,"ITEM_RANGE1"); FOCUS_FIELD(item_range2,"ITEM_RANGE2");
  FOCUS_FIELD(item_azi,"ITEM_AZI"); FOCUS_FIELD(item_elevdo,"ITEM_ELEVDO"); FOCUS_FIELD(item_elevup,"ITEM_ELEVUP");
  FOCUS_FIELD(item_prio,"ITEM_PRIO");
  FOCUS_FIELD(mob_range1,"MOB_RANGE1"); FOCUS_FIELD(mob_range2,"MOB_RANGE2");
  FOCUS_FIELD(mob_azi,"MOB_AZI"); FOCUS_FIELD(mob_elevdo,"MOB_ELEVDO"); FOCUS_FIELD(mob_elevup,"MOB_ELEVUP");
  FOCUS_FIELD(mob_prio,"MOB_PRIO");
#undef FOCUS_FIELD
  }
template<class Instance,class Record,class Visit>
std::string recordLayout(Instance& instance,Record& record,DaedalusVm& vm,Visit visit,unsigned expected) {
  const auto begin=reinterpret_cast<uintptr_t>(&instance);
  const auto recordBegin=reinterpret_cast<uintptr_t>(&record);
  check(reinterpret_cast<uintptr_t>(static_cast<DaedalusInstance*>(&instance))==begin,"VM base comes first");
  check(recordBegin>begin && recordBegin+sizeof(record)<=begin+sizeof(instance),"record base follows VM base within instance");
  auto previousEnd=recordBegin;
  unsigned fields=0;
  std::string signature;
  visit(instance,[&](const char* name,auto& field,bool,const char* binding) {
    ++fields;
    signature+=std::string(name)+":"+binding+" ";
    auto* symbol=vm.find_symbol_by_name(binding);
    check(symbol!=nullptr,std::string("bound record field ")+name);
    check(symbol->registered_to()==typeid(Instance),std::string("concrete VM context for ")+name);
    const auto address=reinterpret_cast<uintptr_t>(&field);
    check(symbol->offset_as_member()==address-begin,std::string("VM offset for ")+name);
    check(address>=previousEnd && address+sizeof(field)<=recordBegin+sizeof(record),std::string("record field order and ownership for ")+name);
    previousEnd=address+sizeof(field);
    auto value=[&](auto& element,uint16_t index) {
      using T=std::remove_reference_t<decltype(element)>;
      if constexpr(std::is_same_v<T,std::string>) {
        symbol->set_string("from VM",index,&instance);
        check(element=="from VM",std::string("VM writes record ")+name);
        element="from record";
        check(symbol->get_string(index,&instance)=="from record",std::string("VM reads record ")+name);
        }
      else if constexpr(std::is_same_v<T,float>) {
        symbol->set_float(1.25f,index,&instance);
        check(element==1.25f,std::string("VM writes record ")+name);
        element=2.5f;
        check(symbol->get_float(index,&instance)==2.5f,std::string("VM reads record ")+name);
        }
      else {
        symbol->set_int(1000+index,index,&instance);
        check(int32_t(element)==1000+index,std::string("VM writes record ")+name);
        element=T(2000+index);
        check(symbol->get_int(index,&instance)==2000+index,std::string("VM reads record ")+name);
        }
      };
    if constexpr(std::is_array_v<std::remove_reference_t<decltype(field)>>) {
      for(uint16_t i=0;i<std::extent_v<std::remove_reference_t<decltype(field)>>;++i) value(field[i],i);
      }
    else value(field,0);
    });
  check(fields==expected,"all record fields checked");
  std::cout<<"Record layout: "<<fields<<" fields, instance size="<<sizeof(instance)
           <<", record size="<<sizeof(record)<<", record offset="<<recordBegin-begin<<'\n';
  return signature;
  }
void itemLayout(const NpcSnapshot::Program& program) {
  auto script=DaedalusScript::from_program(program.data);
  IItem::register_(script);
  DaedalusVm vm(std::move(script));
  auto item=vm.allocate_instance<IItem>("IT_TEST");
  auto& record=static_cast<ItemRecord&>(*item);
  auto signature=recordLayout(*item,record,vm,[](auto& v,auto f) { snapshotFields(v,f); },44);
  check(signature==
    "id:C_ITEM.ID name:C_ITEM.NAME name_id:C_ITEM.NAMEID hp:C_ITEM.HP hp_max:C_ITEM.HP_MAX "
    "main_flag:C_ITEM.MAINFLAG flags:C_ITEM.FLAGS weight:C_ITEM.WEIGHT value:C_ITEM.VALUE "
    "damage_type:C_ITEM.DAMAGETYPE damage_total:C_ITEM.DAMAGETOTAL damage:C_ITEM.DAMAGE wear:C_ITEM.WEAR "
    "protection:C_ITEM.PROTECTION nutrition:C_ITEM.NUTRITION cond_atr:C_ITEM.COND_ATR cond_value:C_ITEM.COND_VALUE "
    "change_atr:C_ITEM.CHANGE_ATR change_value:C_ITEM.CHANGE_VALUE magic:C_ITEM.MAGIC on_equip:C_ITEM.ON_EQUIP "
    "on_unequip:C_ITEM.ON_UNEQUIP on_state:C_ITEM.ON_STATE owner:C_ITEM.OWNER owner_guild:C_ITEM.OWNERGUILD "
    "disguise_guild:C_ITEM.DISGUISEGUILD visual:C_ITEM.VISUAL visual_change:C_ITEM.VISUAL_CHANGE effect:C_ITEM.EFFECT "
    "visual_skin:C_ITEM.VISUAL_SKIN scheme_name:C_ITEM.SCEMENAME material:C_ITEM.MATERIAL munition:C_ITEM.MUNITION "
    "spell:C_ITEM.SPELL range:C_ITEM.RANGE mag_circle:C_ITEM.MAG_CIRCLE description:C_ITEM.DESCRIPTION "
    "text:C_ITEM.TEXT count:C_ITEM.COUNT inv_zbias:C_ITEM.INV_ZBIAS inv_rot_x:C_ITEM.INV_ROTX "
    "inv_rot_y:C_ITEM.INV_ROTY inv_rot_z:C_ITEM.INV_ROTZ inv_animate:C_ITEM.INV_ANIMATE ",
    "original item field order and bindings");
  check(std::extent_v<decltype(record.damage)> == 8,"original item damage array size");
  check(std::extent_v<decltype(record.protection)> == 8,"original item protection array size");
  check(std::extent_v<decltype(record.cond_atr)> == 3,"original item condition attribute array size");
  check(std::extent_v<decltype(record.cond_value)> == 3,"original item condition value array size");
  check(std::extent_v<decltype(record.change_atr)> == 3,"original item change attribute array size");
  check(std::extent_v<decltype(record.change_value)> == 3,"original item change value array size");
  check(std::extent_v<decltype(record.on_state)> == 4,"original item state array size");
  check(std::extent_v<decltype(record.text)> == 6,"original item text array size");
  check(std::extent_v<decltype(record.count)> == 6,"original item count array size");
  }
void focusLayout() {
  check(sizeof(FocusRecord)==76,"original focus record size");
  Fixture f;
  auto cls=f.symbol("C_FOCUS",Type::CLASS);
  f.p.symbols[cls].class_size=sizeof(IFocus);
  IFocus focus{};
  focusFields(focus,[&](const char*,auto& value,bool,const char* binding) {
    auto type=std::is_same_v<std::remove_reference_t<decltype(value)>,float> ? Type::FLOAT : Type::INT;
    f.symbol(binding,type,1,Flag::MEMBER,int32_t(cls));
    });
  f.symbol("FOCUS_TEST",Type::INSTANCE,0,Flag::CONST,int32_t(cls));
  auto script=DaedalusScript::from_program(std::move(f.p));
  IFocus::register_(script);
  DaedalusVm vm(std::move(script));
  auto instance=vm.allocate_instance<IFocus>("FOCUS_TEST");
  recordLayout(*instance,static_cast<FocusRecord&>(*instance),vm,[](auto& v,auto f) { focusFields(v,f); },19);
  }
template<class F> void guildFields(IGuildValues& v,F f) {
  const auto begin=reinterpret_cast<uintptr_t>(static_cast<GuildRecord*>(&v));
#ifdef __APPLE__
  constexpr unsigned textureOffset=8976,turnOffset=10560;
#else
  constexpr unsigned textureOffset=9504,turnOffset=11616;
#endif
#define GUILD_FIELD(name,script,offset) \
  check(reinterpret_cast<uintptr_t>(&v.name)-begin==offset,"original guild offset for " #name); \
  check(std::extent_v<decltype(v.name)> == 66,"original guild array size for " #name); \
  f(#name,v.name,false,"C_GILVALUES." script)
  GUILD_FIELD(water_depth_knee,"WATER_DEPTH_KNEE",0);
  GUILD_FIELD(water_depth_chest,"WATER_DEPTH_CHEST",264);
  GUILD_FIELD(jumpup_height,"JUMPUP_HEIGHT",528);
  GUILD_FIELD(swim_time,"SWIM_TIME",792);
  GUILD_FIELD(dive_time,"DIVE_TIME",1056);
  GUILD_FIELD(step_height,"STEP_HEIGHT",1320);
  GUILD_FIELD(jumplow_height,"JUMPLOW_HEIGHT",1584);
  GUILD_FIELD(jumpmid_height,"JUMPMID_HEIGHT",1848);
  GUILD_FIELD(slide_angle,"SLIDE_ANGLE",2112);
  GUILD_FIELD(slide_angle2,"SLIDE_ANGLE2",2376);
  GUILD_FIELD(disable_autoroll,"DISABLE_AUTOROLL",2640);
  GUILD_FIELD(surface_align,"SURFACE_ALIGN",2904);
  GUILD_FIELD(climb_heading_angle,"CLIMB_HEADING_ANGLE",3168);
  GUILD_FIELD(climb_horiz_angle,"CLIMB_HORIZ_ANGLE",3432);
  GUILD_FIELD(climb_ground_angle,"CLIMB_GROUND_ANGLE",3696);
  GUILD_FIELD(fight_range_base,"FIGHT_RANGE_BASE",3960);
  GUILD_FIELD(fight_range_fist,"FIGHT_RANGE_FIST",4224);
  GUILD_FIELD(fight_range_g,"FIGHT_RANGE_G",4488);
  GUILD_FIELD(fight_range_1hs,"FIGHT_RANGE_1HS",4752);
  GUILD_FIELD(fight_range_1ha,"FIGHT_RANGE_1HA",5016);
  GUILD_FIELD(fight_range_2hs,"FIGHT_RANGE_2HS",5280);
  GUILD_FIELD(fight_range_2ha,"FIGHT_RANGE_2HA",5544);
  GUILD_FIELD(falldown_height,"FALLDOWN_HEIGHT",5808);
  GUILD_FIELD(falldown_damage,"FALLDOWN_DAMAGE",6072);
  GUILD_FIELD(blood_disabled,"BLOOD_DISABLED",6336);
  GUILD_FIELD(blood_max_distance,"BLOOD_MAX_DISTANCE",6600);
  GUILD_FIELD(blood_amount,"BLOOD_AMOUNT",6864);
  GUILD_FIELD(blood_flow,"BLOOD_FLOW",7128);
  GUILD_FIELD(blood_emitter,"BLOOD_EMITTER",7392);
  GUILD_FIELD(blood_texture,"BLOOD_TEXTURE",textureOffset);
  GUILD_FIELD(turn_speed,"TURN_SPEED",turnOffset);
#undef GUILD_FIELD
  }
void guildLayout() {
#ifdef __APPLE__
  check(sizeof(GuildRecord)==10824 && sizeof(IGuildValues)==10856,"original Mac guild sizes");
#else
  check(sizeof(GuildRecord)==11880 && sizeof(IGuildValues)==11912,"original Linux guild sizes");
#endif
  check(GuildRecord::count==66,"original guild count");
  Fixture f;
  auto cls=f.symbol("C_GILVALUES",Type::CLASS);
  f.p.symbols[cls].class_size=sizeof(IGuildValues);
  IGuildValues value{};
  guildFields(value,[&](const char*,auto& field,bool,const char* binding) {
    using T=std::remove_all_extents_t<std::remove_reference_t<decltype(field)>>;
    f.symbol(binding,std::is_same_v<T,std::string> ? Type::STRING : Type::INT,66,Flag::MEMBER,int32_t(cls));
    });
  f.symbol("GUILD_TEST",Type::INSTANCE,0,Flag::CONST,int32_t(cls));
  auto script=DaedalusScript::from_program(std::move(f.p));
  IGuildValues::register_(script);
  DaedalusVm vm(std::move(script));
  auto instance=vm.allocate_instance<IGuildValues>("GUILD_TEST");
  auto& record=static_cast<GuildRecord&>(*instance);
  check(reinterpret_cast<uintptr_t>(&record)-reinterpret_cast<uintptr_t>(instance.get())==32,"original guild record base offset");
  recordLayout(*instance,record,vm,[](auto& v,auto f) { guildFields(v,f); },31);
  }
template<class F> void spellFields(ISpell& v,F f) {
  const auto begin=reinterpret_cast<uintptr_t>(static_cast<SpellRecord*>(&v));
#define SPELL_FIELD(name,script,offset) \
  check(reinterpret_cast<uintptr_t>(&v.name)-begin==offset,"original spell offset for " #name); \
  f(#name,v.name,false,"C_SPELL." script)
  SPELL_FIELD(time_per_mana,"TIME_PER_MANA",0);
  SPELL_FIELD(damage_per_level,"DAMAGE_PER_LEVEL",4);
  SPELL_FIELD(damage_type,"DAMAGETYPE",8);
  SPELL_FIELD(spell_type,"SPELLTYPE",12);
  SPELL_FIELD(can_turn_during_invest,"CANTURNDURINGINVEST",16);
  SPELL_FIELD(can_change_target_during_invest,"CANCHANGETARGETDURINGINVEST",20);
  SPELL_FIELD(is_multi_effect,"ISMULTIEFFECT",24);
  SPELL_FIELD(target_collect_algo,"TARGETCOLLECTALGO",28);
  SPELL_FIELD(target_collect_type,"TARGETCOLLECTTYPE",32);
  SPELL_FIELD(target_collect_range,"TARGETCOLLECTRANGE",36);
  SPELL_FIELD(target_collect_azi,"TARGETCOLLECTAZI",40);
  SPELL_FIELD(target_collect_elev,"TARGETCOLLECTELEV",44);
#undef SPELL_FIELD
  }
void spellLayout() {
  check(sizeof(SpellRecord)==48 && sizeof(ISpell)==80,"original spell sizes");
  Fixture f;
  auto cls=f.symbol("C_SPELL",Type::CLASS);
  f.p.symbols[cls].class_size=sizeof(ISpell);
  ISpell value{};
  spellFields(value,[&](const char*,auto& field,bool,const char* binding) {
    auto type=std::is_same_v<std::remove_reference_t<decltype(field)>,float> ? Type::FLOAT : Type::INT;
    f.symbol(binding,type,1,Flag::MEMBER,int32_t(cls));
    });
  f.symbol("SPELL_TEST",Type::INSTANCE,0,Flag::CONST,int32_t(cls));
  auto script=DaedalusScript::from_program(std::move(f.p));
  ISpell::register_(script);
  DaedalusVm vm(std::move(script));
  auto instance=vm.allocate_instance<ISpell>("SPELL_TEST");
  auto& record=static_cast<SpellRecord&>(*instance);
  check(reinterpret_cast<uintptr_t>(&record)-reinterpret_cast<uintptr_t>(instance.get())==32,"original spell record base offset");
  recordLayout(*instance,record,vm,[](auto& v,auto f) { spellFields(v,f); },12);
  }
void itemSelection(const fs::path& dir) {
  const auto base=dir/"selection-fixture";
  auto selected=itemProgram(),other=selected;
  bool changed=false;
  for(auto& symbol:other.data.symbols)
    if(!symbol.string_values.empty() && symbol.string_values[0].starts_with("name0")) {
      symbol.string_values[0]="another program";
      changed=true;
      }
  check(changed,"selection fixture has distinct program names");
  auto encoded=ProgramData::encodeProgram(other.data);
  other.name=encoded.name; other.sha256=encoded.sha256;
  check(other.sha256!=selected.sha256,"selection programs have different identities");
  const auto programs=base/"programs";
  fs::create_directories(programs);
  for(const auto* program:{&selected,&other}) {
    ProgramData::writeProgram(program->data,programs);
    NpcSnapshot::writeItems(*program,base/"definitions"/program->sha256/"items.json");
    }
  std::string selectedSha,otherSha;
  auto program=DaedalusScript::from_program(ProgramData::readProgram(programs/(selected.name+".json"),&selectedSha));
  auto otherProgram=DaedalusScript::from_program(ProgramData::readProgram(programs/(other.name+".json"),&otherSha));
  check(selectedSha==selected.sha256 && otherSha==other.sha256,"selection uses the verified loaded record identities");
  const auto symbol=program.find_symbol_by_name("IT_RECORD_TEST")->index();
  const auto expected=ItemDefinitions::read(base,selected.sha256).items.at("IT_RECORD_TEST").record.name;
  const char* previous=std::getenv("OPENGOTHIC_ITEM_RECORDS");
  const bool hadPrevious=previous!=nullptr;
  const std::string saved=previous ? previous : "";
  auto environment=[](const char* value) {
#ifdef _WIN32
    check(_putenv_s("OPENGOTHIC_ITEM_RECORDS",value ? value : "")==0,"set item records environment");
#else
    check((value ? setenv("OPENGOTHIC_ITEM_RECORDS",value,1) : unsetenv("OPENGOTHIC_ITEM_RECORDS"))==0,
          "set item records environment");
#endif
    };
  try {
    environment(nullptr);
    auto defaults=ItemDefinitionRuntime::load(base,program,selectedSha);
    check(defaults.items.contains(symbol),"item records enabled by default");
    check(defaults.items.at(symbol).name==expected,"default section uses the loaded program SHA");
    environment("0");
    check(ItemDefinitionRuntime::load(base/"absent",program,selectedSha).items.empty(),"zero disables records without reading the base");
    environment("1");
    auto enabled=ItemDefinitionRuntime::load(base,program,selectedSha);
    check(enabled.items.contains(symbol),"nonzero enables item records");
    check(enabled.items.at(symbol).name==expected,"enabled section uses the loaded program SHA");
    check(ItemDefinitionRuntime::load(base,otherProgram,otherSha).items.at(symbol).name=="another program",
          "changing the loaded program changes the selected section");
    }
  catch(...) { environment(hadPrevious ? saved.c_str() : nullptr); throw; }
  environment(hadPrevious ? saved.c_str() : nullptr);
  }
void fixture(const fs::path& dir) {
  auto p=Fixture().make();
  npcLayout(p);
  itemLayout(p);
  focusLayout();
  guildLayout();
  spellLayout();
  itemDefinitions(dir);
  npcDefinitions(dir);
  itemSelection(dir);
  routineDefinitions(dir);
  routineSelection(dir);
  auto a=dir/"a",b=dir/"b";
  NpcSnapshot::write(p,a); check(NpcSnapshot::verify(p,a)==0,"fixture verifies");
  NpcSnapshot::write(p,b);
  for(auto name:{"npcs.json","items.json","routines.json","population.json","report.json"}) {
    std::ifstream x(a/name,std::ios::binary),y(b/name,std::ios::binary);
    check(std::string(std::istreambuf_iterator<char>(x),{})==std::string(std::istreambuf_iterator<char>(y),{}),"byte-identical snapshots");
    }
  auto npc=read(a/"npcs.json"); auto& records=npc["records"];
  check(records.Size()==2,"two NPCs");
  check(std::string(records[0]["reason"].GetString())=="game_state","global dependency refused");
  check(std::string(records[0]["detail"].GetString())=="CHAPTER","global dependency names the variable");
  auto& good=records[1];
  check(good["fields"]["id"].GetInt()==123 && good["fields"]["attribute"][0].GetInt()==42,"prototype and helper executed");
  check(std::string(good["fields"]["daily_routine"].GetString())=="RTN_START_123","function field uses symbol name");
  check(good["calls"].Size()==4 && good["calls"][3]["deferred"].GetBool(),"random helper is deferred transitively");
  check(std::string(good["calls"][3]["arguments"][0].GetString())=="self","deferred arguments retained");
  auto items=read(a/"items.json"); check(items["records"][0]["fields"]["value"].GetInt()==9,"item fields");
  auto routines=read(a/"routines.json"); check(routines["records"][0]["rows"].Size()==2,"two routine rows");
  check(std::string(routines["records"][0]["rows"][0]["state"].GetString())=="ZS_TEST","routine state symbol");
  auto population=read(a/"population.json");
  for(unsigned i=0;i<2;++i) {
    check(population["records"][i]["calls"].Size()==1 && population["records"][i].HasMember("reason"),"safe startup prefix before state branch");
    check(std::string(population["records"][i]["detail"].GetString())=="CHAPTER before state branch","startup stops at the state branch");
    }
  check(population["records"][2]["calls"].Size()==4,"all four population APIs");
  auto report=read(a/"report.json");
  check(report["deferred_calls"]["AMBIENT"].GetInt()==1 && report["uncaptured"].Size()==3,"report reasons and deferred counts");

  good["fields"]["attribute"][0].SetInt(43);
  write(b/"npcs.json",npc);
  check(NpcSnapshot::verify(p,b)>0,"verification rejects a changed attribute");
  good["fields"]["attribute"][0].SetInt(42);
  good["calls"][0].Swap(good["calls"][1]);
  write(b/"npcs.json",npc);
  check(NpcSnapshot::verify(p,b)>0,"verification rejects reordered constructor calls");
  good["calls"][0].Swap(good["calls"][1]);
  write(b/"npcs.json",npc);
  check(NpcSnapshot::verify(p,b)==0,"restored snapshot verifies");
  routines["records"][0]["rows"].PopBack();
  write(b/"routines.json",routines);
  check(NpcSnapshot::verify(p,b)>0,"verification rejects a missing routine row");
  }
fs::path readyBase() {
  if(auto* p=std::getenv("OPENGOTHIC_BASE"); p && *p) return p;
  fs::path cache;
  if(auto* p=std::getenv("OPENGOTHIC_BASE_CACHE"); p && *p) cache=p;
  else if(auto* p=std::getenv("HOME")) cache=fs::path(p)/".cache/gothic-ai/base";
  auto* fingerprint=std::getenv("OPENGOTHIC_BASE_FINGERPRINT");
  if(fingerprint && *fingerprint) return cache/fingerprint;
  // Match BaseStore's installation-specific pointer; unrelated fixture pointers are irrelevant.
  const auto* game=std::getenv("GOTHIC_DIR");
  if(!game || !*game) return {};
  const auto gothic=fs::weakly_canonical(game).generic_string();
  const auto file=cache/"current"/(BaseImport::sha256(gothic)+".json");
  if(!fs::is_regular_file(file)) return {};
  auto pointer=read(file);
  if(!pointer.IsObject() || !pointer.HasMember("version") || pointer["version"]!=1 ||
     !pointer.HasMember("gothic") || !pointer["gothic"].IsString() || pointer["gothic"].GetString()!=gothic ||
     !pointer.HasMember("base") || !pointer["base"].IsObject()) return {};
  const auto& base=pointer["base"];
  if(base.HasMember("path") && base["path"].IsString()) {
    fs::path path=base["path"].GetString();
    if(path.is_absolute() && fs::is_regular_file(path/"programs/programs.json")) return path;
    }
  return {};
  }
}
int main() {
  auto dir=fs::temp_directory_path()/("gothic-npc-snapshot-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  try {
    fs::create_directories(dir);
    fixture(dir);
    auto base=readyBase();
    if(base.empty()) std::cout<<"SKIP real corpus: no ready base selected (OPENGOTHIC_BASE or the GOTHIC_DIR current-base pointer)\n";
    else {
      auto program=NpcSnapshot::load(base);
      NpcSnapshot::write(program,dir/"real");
      check(NpcSnapshot::verify(program,dir/"real")==0,"real corpus verifies");
      runtimeItems(program,base);
      }
    fs::remove_all(dir);
    std::cout<<"NPC snapshot contract passed: "<<assertions<<" assertions\n";
    return 0;
    }
  catch(const std::exception& e) { fs::remove_all(dir); std::cerr<<"NPC snapshot contract: "<<e.what()<<'\n'; return 1; }
  }

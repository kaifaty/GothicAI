#include "npcsnapshot.h"
#include "daedalus/classes.h"
#include "daedalus/vm.h"
#include "npcsnapshotfields.h"
#include "programdata.h"
#include <rapidjson/document.h>
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
    auto init=fn("INIT_STATE"); begin(init);
    op(Op::PUSHI,npc); op(Op::PUSHV,point); op(Op::BE,insert);
    op(Op::PUSHV,global); op(Op::BZ,uint32_t(p.bytecode.size()+6)); op(Op::RSR); op(Op::RSR);
    auto encoded=ProgramData::encodeProgram(p);
    return {std::move(p),encoded.name,encoded.sha256};
    }
  };
rapidjson::Document read(const fs::path& path) {
  std::ifstream f(path); std::string s{std::istreambuf_iterator<char>(f),{}};
  rapidjson::Document j; j.Parse(s.c_str()); check(!j.HasParseError(),"valid fixture JSON"); return j;
  }
void fixture(const fs::path& dir) {
  auto p=Fixture().make();
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
  auto& good=records[1];
  check(good["fields"]["id"].GetInt()==123 && good["fields"]["attribute"][0].GetInt()==42,"prototype and helper executed");
  check(std::string(good["fields"]["daily_routine"].GetString())=="RTN_START_123","function field uses symbol name");
  check(good["calls"].Size()==4 && good["calls"][3]["deferred"].GetBool(),"random helper is deferred transitively");
  check(std::string(good["calls"][3]["arguments"][0].GetString())=="self","deferred arguments retained");
  auto items=read(a/"items.json"); check(items["records"][0]["fields"]["value"].GetInt()==9,"item fields");
  auto routines=read(a/"routines.json"); check(routines["records"][0]["rows"].Size()==2,"two routine rows");
  check(std::string(routines["records"][0]["rows"][0]["state"].GetString())=="ZS_TEST","routine state symbol");
  auto population=read(a/"population.json");
  check(population["records"][0]["calls"].Size()==1 && population["records"][0].HasMember("reason"),"safe startup prefix before state branch");
  check(population["records"][1]["calls"].Size()==4,"all four population APIs");
  auto report=read(a/"report.json");
  check(report["deferred_calls"]["AMBIENT"].GetInt()==1 && report["uncaptured"].Size()==2,"report reasons and deferred counts");
  }
fs::path readyBase() {
  if(auto* p=std::getenv("OPENGOTHIC_BASE"); p && *p) return p;
  fs::path cache;
  if(auto* p=std::getenv("OPENGOTHIC_BASE_CACHE"); p && *p) cache=p;
  else if(auto* p=std::getenv("HOME")) cache=fs::path(p)/".cache/gothic-ai/base";
  auto* fingerprint=std::getenv("OPENGOTHIC_BASE_FINGERPRINT");
  if(fingerprint && *fingerprint) return cache/fingerprint;
  // Without an explicit selection, use the ready base selected by the installation pointer.
  if(!fs::is_directory(cache/"current")) return {};
  fs::path newest;
  for(auto& f:fs::directory_iterator(cache/"current"))
    if(f.path().extension()==".json" && (newest.empty() || fs::last_write_time(f.path())>fs::last_write_time(newest))) newest=f.path();
  if(newest.empty()) return {};
  auto pointer=read(newest);
  if(pointer.HasMember("base") && pointer["base"].HasMember("path")) return pointer["base"]["path"].GetString();
  return {};
  }
}
int main() {
  auto dir=fs::temp_directory_path()/("gothic-npc-snapshot-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  try {
    fs::create_directories(dir);
    fixture(dir);
    auto base=readyBase();
    if(base.empty()) std::cout<<"SKIP real corpus: no ready base selected (OPENGOTHIC_BASE or current-base pointer)\n";
    else {
      auto program=NpcSnapshot::load(base);
      NpcSnapshot::write(program,dir/"real");
      check(NpcSnapshot::verify(program,dir/"real")==0,"real corpus verifies");
      }
    fs::remove_all(dir);
    std::cout<<"NPC snapshot contract passed: "<<assertions<<" assertions\n";
    return 0;
    }
  catch(const std::exception& e) { fs::remove_all(dir); std::cerr<<"NPC snapshot contract: "<<e.what()<<'\n'; return 1; }
  }

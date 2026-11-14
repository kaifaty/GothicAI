#include "npcsnapshot.h"
#include "daedalus/classes.h"
#include "daedalus/vm.h"
#include "npcsnapshotfields.h"
#include "programdata.h"
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
  bool rejected=false;
  try { vm.find_symbol_by_name("C_NPC.ID")->set_int(1,0,item.get()); }
  catch(const DaedalusIllegalContextType&) { rejected=true; }
  check(rejected,"NPC member rejects an item context");
  std::cout<<"NPC layout: "<<fields<<" fields, sizeof(INpc)="<<sizeof(INpc)
           <<", sizeof(NpcRecord)="<<sizeof(NpcRecord)<<", record offset="<<recordBegin-begin<<'\n';
  }
void fixture(const fs::path& dir) {
  auto p=Fixture().make();
  npcLayout(p);
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
      }
    fs::remove_all(dir);
    std::cout<<"NPC snapshot contract passed: "<<assertions<<" assertions\n";
    return 0;
    }
  catch(const std::exception& e) { fs::remove_all(dir); std::cerr<<"NPC snapshot contract: "<<e.what()<<'\n'; return 1; }
  }

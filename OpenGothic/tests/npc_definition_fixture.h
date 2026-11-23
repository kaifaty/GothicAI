#pragma once

#include "npcsnapshot.h"
#include "daedalus/classes.h"
#include "daedalus/vm.h"
#include "npcsnapshotfields.h"
#include "programdata.h"

#include <array>
#include <bit>
#include <stdexcept>
#include <type_traits>

namespace NpcDefinitionFixture {
using namespace daedalus;
using Op=DaedalusOpcode;
using Type=DaedalusDataType;
namespace Flag=DaedalusSymbolFlag;
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
  };
// D4.1: routine states of the activity template with Gothic 2's own bytecode, names and external
// signatures. ZS_SMITH_ANVIL and ZS_SMOKE_WATERPIPE (the distance approach) are accepted, ZS_TEST_ROAST is
// captured but not accepted, ZS_SMITH_SWAPPED is refused.
inline void activities(Fixture& f,uint32_t self,uint32_t perceptionHandler,uint32_t ta,uint32_t point) {
  auto constant=[&](const char* name,int value) {
    const auto id=f.symbol(name,Type::INT,1,Flag::CONST); f.p.symbols[id].int_values[0]=value; return id;
    };
  auto text=[&](const char* value) {
    const auto id=f.symbol("$ACTIVITY_"+std::to_string(f.p.symbols.size()),Type::STRING,1,Flag::CONST);
    f.p.symbols[id].string_values[0]=value; return id;
    };
  auto jump=[&](Op op) { f.op(op,0u); return f.p.bytecode.size()-4; };
  auto land=[&](size_t at) {
    const auto to=uint32_t(f.p.bytecode.size());
    for(size_t i=0;i<4;++i) f.p.bytecode[at+i]=std::byte((to>>(8*i))&255);
    };
  const auto walk=constant("NPC_WALK",1),no=constant("FALSE",0),yes=constant("TRUE",1);
  const auto busy=constant("BS_MOBINTERACT_INTERRUPT",32784),loopContinue=constant("LOOP_CONTINUE",0);
  const auto freeHands=constant("BS_FLAG_FREEHANDS",65536),interruptable=constant("BS_FLAG_INTERRUPTABLE",32768);
  const auto maxState=constant("BS_MAX",31);
  const auto wp=f.find("C_NPC.WP");
  const auto percEnable=f.fn("NPC_PERCENABLE",{Type::INSTANCE,Type::INT,Type::FUNCTION},true);
  const auto lookAt=f.fn("NPC_GETLOOKATTARGET",{Type::INSTANCE},true,Type::INSTANCE);
  const auto valid=f.fn("HLP_ISVALIDNPC",{Type::INSTANCE},true,Type::INT);
  const auto stopLook=f.fn("AI_STOPLOOKAT",{Type::INSTANCE},true);
  const auto removeWeapon=f.fn("AI_REMOVEWEAPON",{Type::INSTANCE},true);
  const auto setWalk=f.fn("AI_SETWALKMODE",{Type::INSTANCE,Type::INT},true);
  const auto nearest=f.fn("NPC_GETNEARESTWP",{Type::INSTANCE},true,Type::STRING);
  const auto compare=f.fn("HLP_STRCMP",{Type::STRING,Type::STRING},true,Type::INT);
  const auto gotoWp=f.fn("AI_GOTOWP",{Type::INSTANCE,Type::STRING},true);
  const auto distToWp=f.fn("NPC_GETDISTTOWP",{Type::INSTANCE,Type::STRING},true,Type::INT);
  const auto available=f.fn("WLD_ISMOBAVAILABLE",{Type::INSTANCE,Type::STRING},true,Type::INT);
  const auto bodyState=f.fn("NPC_GETBODYSTATE",{Type::INSTANCE},true,Type::INT);
  const auto useMob=f.fn("AI_USEMOB",{Type::INSTANCE,Type::STRING,Type::INT},true,Type::INT);

  const auto perceptions=f.fn("PERCEPTION_SET_NORMAL"); f.begin(perceptions);
  f.op(Op::PUSHI,3u); f.op(Op::GMOVI,self); f.op(Op::PUSHV,f.find("C_NPC.SENSES")); f.op(Op::MOVI);
  f.op(Op::PUSHVI,self); f.op(Op::PUSHI,1u); f.op(Op::PUSHI,perceptionHandler); f.op(Op::BE,percEnable); f.op(Op::RSR);
  const auto stopLookAt=f.fn("B_STOPLOOKAT",{Type::INSTANCE});
  const auto target=f.symbol("B_STOPLOOKAT.TARGET",Type::INSTANCE,1,0,int32_t(f.find("C_NPC")));
  f.begin(stopLookAt); f.op(Op::PUSHVI,stopLookAt+1); f.op(Op::MOVVI);
  f.op(Op::PUSHVI,stopLookAt+1); f.op(Op::BE,lookAt); f.op(Op::PUSHVI,target); f.op(Op::MOVVI);
  f.op(Op::PUSHVI,target); f.op(Op::BE,valid);
  auto skipStop=jump(Op::BZ);
  f.op(Op::PUSHVI,stopLookAt+1); f.op(Op::BE,stopLook);
  land(skipStop); f.op(Op::RSR);
  const auto reset=f.fn("B_RESETALL",{Type::INSTANCE}); f.begin(reset);
  f.op(Op::PUSHVI,reset+1); f.op(Op::MOVVI);
  f.op(Op::PUSHVI,reset+1); f.call(stopLookAt);
  f.op(Op::PUSHVI,reset+1); f.op(Op::BE,removeWeapon); f.op(Op::RSR);
  const auto contains=f.fn("C_BODYSTATECONTAINS",{Type::INSTANCE,Type::INT},false,Type::INT); f.begin(contains);
  f.op(Op::PUSHV,contains+2); f.op(Op::MOVI); f.op(Op::PUSHVI,contains+1); f.op(Op::MOVVI);
  auto mask=[&] { f.op(Op::PUSHV,freeHands); f.op(Op::PUSHV,interruptable); f.op(Op::PUSHV,maxState); f.op(Op::OR); f.op(Op::OR); };
  mask(); f.op(Op::PUSHV,contains+2); f.op(Op::ANDB);
  mask(); f.op(Op::PUSHVI,contains+1); f.op(Op::BE,bodyState); f.op(Op::ANDB); f.op(Op::EQ);
  auto notContained=jump(Op::BZ);
  f.op(Op::PUSHV,yes); f.op(Op::RSR);
  auto after=jump(Op::B);
  land(notContained); f.op(Op::PUSHV,no); f.op(Op::RSR);
  land(after); f.op(Op::RSR);

  // `walkFirst` sets the walk mode before B_ResetAll: correct phases, one call out of the template's order.
  // `distance` > 0 walks when Npc_GetDistToWP(self,self.wp) is greater, as ZS_Smoke_Waterpipe does.
  auto mobState=[&](const std::string& name,const char* scheme,bool walkFirst=false,uint32_t distance=0) {
    const auto init=f.fn(name.c_str()); f.begin(init);
    f.call(perceptions);
    if(walkFirst) { f.op(Op::PUSHVI,self); f.op(Op::PUSHV,walk); f.op(Op::BE,setWalk); }
    f.op(Op::PUSHVI,self); f.call(reset);
    if(!walkFirst) { f.op(Op::PUSHVI,self); f.op(Op::PUSHV,walk); f.op(Op::BE,setWalk); }
    if(distance==0) {
      f.op(Op::PUSHV,no); f.op(Op::PUSHVI,self); f.op(Op::BE,nearest);
      f.op(Op::GMOVI,self); f.op(Op::PUSHV,wp); f.op(Op::BE,compare); f.op(Op::EQ);
      } else {
      f.op(Op::PUSHI,distance); f.op(Op::PUSHVI,self); f.op(Op::GMOVI,self); f.op(Op::PUSHV,wp); f.op(Op::BE,distToWp);
      f.op(Op::GT);
      }
    auto atPoint=jump(Op::BZ);
    f.op(Op::PUSHVI,self); f.op(Op::GMOVI,self); f.op(Op::PUSHV,wp); f.op(Op::BE,gotoWp);
    land(atPoint); f.op(Op::RSR);
    const auto loop=f.fn((name+"_LOOP").c_str(),{},false,Type::INT); f.begin(loop);
    f.op(Op::PUSHVI,self); f.op(Op::PUSHV,text(scheme)); f.op(Op::BE,available);
    f.op(Op::PUSHVI,self); f.op(Op::PUSHV,busy); f.call(contains); f.op(Op::NOT); f.op(Op::AND);
    auto skipUse=jump(Op::BZ);
    f.op(Op::PUSHVI,self); f.op(Op::PUSHV,text(scheme)); f.op(Op::PUSHI,1u); f.op(Op::BE,useMob);
    land(skipUse); f.op(Op::PUSHV,loopContinue); f.op(Op::RSR); f.op(Op::RSR);
    const auto end=f.fn((name+"_END").c_str()); f.begin(end);
    f.op(Op::PUSHVI,self); f.op(Op::PUSHV,text(scheme)); f.op(Op::PUSHI,1u); f.op(Op::NEGATE); f.op(Op::BE,useMob); f.op(Op::RSR);
    return init;
    };
  const auto anvil=mobState("ZS_SMITH_ANVIL","BSANVIL");
  const auto roast=mobState("ZS_TEST_ROAST","BARBQ");
  const auto swapped=mobState("ZS_SMITH_SWAPPED","BSCOOL",true);
  const auto pipe=mobState("ZS_SMOKE_WATERPIPE","SMOKE",false,500);
  const auto routine=f.fn("RTN_SMITH_7"); f.begin(routine);
  for(auto [from,to,state]:{std::tuple{8u,12u,anvil},std::tuple{12u,16u,roast},std::tuple{16u,20u,swapped},std::tuple{20u,24u,pipe}}) {
    f.op(Op::PUSHVI,self); f.op(Op::PUSHI,from); f.op(Op::PUSHI,0u); f.op(Op::PUSHI,to); f.op(Op::PUSHI,0u);
    f.op(Op::PUSHI,state); f.op(Op::PUSHV,point); f.op(Op::BE,ta);
    }
  f.op(Op::RSR);
  }

// The six deferred helper forms, with live equipment effects and two ordered ambient items.
// Their bytecode goes through the same import recognizer as an installed program.
struct Helpers {
  uint32_t fight, add, visual, ambient, skeleton, attributes, precedingRandom, head;
  };
inline Helpers helpers(Fixture& f,uint32_t npcClass,uint32_t self,uint32_t random,uint32_t create,
                       uint32_t armor,uint32_t ambientItem,uint32_t trailerItem) {
  auto constant=[&](const char* name,int value) {
    const auto id=f.symbol(name,Type::INT,1,Flag::CONST); f.p.symbols[id].int_values[0]=value; return id;
    };
  auto text=[&](const char* value) {
    const auto id=f.symbol("$HELPER_"+std::to_string(f.p.symbols.size()),Type::STRING,1,Flag::CONST);
    f.p.symbols[id].string_values[0]=value; return id;
    };
  auto function=[&](const char* name,std::initializer_list<Type> types,std::initializer_list<const char*> names) {
    const auto id=f.fn(name,types);
    uint32_t parameter=id;
    for(const auto* local:names) {
      auto& symbol=f.p.symbols[++parameter]; symbol.name=std::string(name)+"."+local;
      if(symbol.type==uint32_t(Type::INSTANCE)) symbol.parent=int32_t(npcClass);
      }
    return id;
    };
  auto parameters=[&](uint32_t fn) {
    for(auto i=f.p.symbols[fn].count;i>0;--i) {
      const auto type=Type(f.p.symbols[fn+i].type);
      f.op(type==Type::INSTANCE ? Op::PUSHVI : Op::PUSHV,fn+i);
      f.op(type==Type::INSTANCE ? Op::MOVVI : type==Type::STRING ? Op::MOVS : Op::MOVI);
      }
    };
  auto jump=[&](Op op) { f.op(op,0u); return f.p.bytecode.size()-4; };
  auto land=[&](size_t at) {
    const auto to=uint32_t(f.p.bytecode.size());
    for(size_t i=0;i<4;++i) f.p.bytecode[at+i]=std::byte((to>>(8*i))&255);
    };
  auto member=[&](uint32_t npc,const char* name,uint8_t index=0) {
    f.op(Op::GMOVI,npc); f.op(Op::PUSHVV,f.find(name)); f.p.bytecode.push_back(std::byte(index));
    };
  auto assign=[&](uint32_t npc,const char* name,uint8_t index,int value) {
    f.op(Op::PUSHI,uint32_t(value)); member(npc,name,index); f.op(Op::MOVI);
    };
  const std::array<uint32_t,4> talents={constant("NPC_TALENT_1H",1),constant("NPC_TALENT_2H",2),
      constant("NPC_TALENT_BOW",3),constant("NPC_TALENT_CROSSBOW",4)};
  constant("MALE",0); constant("ATR_STRENGTH",4); constant("AIV_GENDER",17);
  const auto skill=f.fn("NPC_SETTALENTSKILL",{Type::INSTANCE,Type::INT,Type::INT},true);
  Helpers h{};
  h.add=function("B_ADDFIGHTSKILL",{Type::INSTANCE,Type::INT,Type::INT},{"SLF","TALENT","PERCENT"});
  f.begin(h.add); parameters(h.add);
  for(uint8_t i=1;i<=4;++i) {
    f.op(Op::PUSHV,talents[i-1]); f.op(Op::PUSHV,h.add+2); f.op(Op::EQ); const auto next=jump(Op::BZ);
    f.op(Op::PUSHV,h.add+3); member(h.add+1,"C_NPC.HITCHANCE",i); f.op(Op::ADD);
    member(h.add+1,"C_NPC.HITCHANCE",i); f.op(Op::MOVI);
    for(uint32_t tier=0;tier<3;++tier) {
      f.op(Op::PUSHI,tier*30); member(h.add+1,"C_NPC.HITCHANCE",tier==0 && i==2 ? 1 : i);
      f.op(Op::GTE); const auto skip=jump(Op::BZ);
      f.op(Op::PUSHVI,h.add+1); f.op(Op::PUSHV,talents[i-1]); f.op(Op::PUSHI,tier); f.op(Op::BE,skill); land(skip);
      }
    land(next);
    }
  f.op(Op::RSR);
  const auto real=function("B_RAISEREALFIGHTTALENTPERCENT",{Type::INSTANCE,Type::INT,Type::INT},{"OTH","TALENT","PERCENT"});
  f.begin(real); parameters(real);
  for(uint8_t i=1;i<=4;++i) {
    constant(("AIV_TALENT_"+std::to_string(i)).c_str(),83+i);
    f.op(Op::PUSHV,talents[i-1]); f.op(Op::PUSHV,real+2); f.op(Op::EQ); const auto next=jump(Op::BZ);
    f.op(Op::PUSHV,real+3); member(real+1,"C_NPC.AIVAR",uint8_t(83+i)); f.op(Op::ADD);
    member(real+1,"C_NPC.AIVAR",uint8_t(83+i)); f.op(Op::MOVI); land(next);
    }
  f.op(Op::RSR);
  const auto raise=function("B_RAISEFIGHTTALENT",{Type::INSTANCE,Type::INT,Type::INT},{"OTH","TALENT","PERCENT"});
  f.begin(raise); parameters(raise);
  for(const auto target:{real,h.add}) {
    f.op(Op::PUSHVI,raise+1); f.op(Op::PUSHV,raise+2); f.op(Op::PUSHV,raise+3); f.call(target);
    }
  f.op(Op::RSR);
  h.fight=function("B_SETFIGHTSKILLS",{Type::INSTANCE,Type::INT},{"SLF","PERCENT"});
  f.begin(h.fight); parameters(h.fight);
  for(const auto talent:talents) {
    f.op(Op::PUSHVI,h.fight+1); f.op(Op::PUSHV,talent); f.op(Op::PUSHV,h.fight+2); f.call(raise);
    }
  f.op(Op::RSR);
  const auto model=f.fn("MDL_SETVISUAL",{Type::INSTANCE,Type::STRING},true);
  const auto body=f.fn("MDL_SETVISUALBODY",{Type::INSTANCE,Type::STRING,Type::INT,Type::INT,Type::STRING,Type::INT,Type::INT,Type::INT},true);
  const auto scale=f.fn("MDL_SETMODELSCALE",{Type::INSTANCE,Type::FLOAT,Type::FLOAT,Type::FLOAT},true);
  const auto overlay=f.fn("MDL_APPLYOVERLAYMDS",{Type::INSTANCE,Type::STRING},true);
  const auto humans=text("HUMANS.MDS"),maleBody=text("hum_body_Naked0"),femaleBody=text("Hum_Body_Babe0");
  h.head=text("");
  h.visual=function("B_SETNPCVISUAL",{Type::INSTANCE,Type::INT,Type::STRING,Type::INT,Type::INT,Type::INT},
                    {"SLF","GENDER","HEADMESH","FACETEX","BODYTEX","ARMORINSTANCE"});
  f.begin(h.visual); parameters(h.visual);
  f.op(Op::PUSHV,h.visual+2); member(h.visual+1,"C_NPC.AIVAR",17); f.op(Op::MOVI);
  f.op(Op::PUSHVI,h.visual+1); f.op(Op::PUSHV,humans); f.op(Op::BE,model);
  f.op(Op::PUSHI,0u); f.op(Op::PUSHV,h.visual+2); f.op(Op::EQ); const auto female=jump(Op::BZ);
  auto visualBody=[&](uint32_t mesh) {
    f.op(Op::PUSHVI,h.visual+1); f.op(Op::PUSHV,mesh); f.op(Op::PUSHV,h.visual+5); f.op(Op::PUSHI,0u);
    f.op(Op::PUSHV,h.visual+3); f.op(Op::PUSHV,h.visual+4); f.op(Op::PUSHI,0u); f.op(Op::PUSHV,h.visual+6); f.op(Op::BE,body);
    };
  visualBody(maleBody);
  for(const auto& choice:{std::pair{50,0.9f},std::pair{100,1.1f}}) {
    f.op(Op::PUSHI,uint32_t(choice.first)); member(h.visual+1,"C_NPC.ATTRIBUTE",4);
    f.op(choice.first==50 ? Op::LT : Op::GT); const auto skip=jump(Op::BZ);
    f.op(Op::PUSHVI,h.visual+1); f.op(Op::PUSHI,std::bit_cast<uint32_t>(choice.second));
    f.op(Op::PUSHI,std::bit_cast<uint32_t>(1.f)); f.op(Op::PUSHI,std::bit_cast<uint32_t>(1.f)); f.op(Op::BE,scale); land(skip);
    }
  const auto done=jump(Op::B); land(female);
  f.op(Op::PUSHI,3u); f.op(Op::PUSHV,h.visual+5); f.op(Op::LTE);
  f.op(Op::PUSHI,0u); f.op(Op::PUSHV,h.visual+5); f.op(Op::GTE); f.op(Op::AND); const auto textureDone=jump(Op::BZ);
  f.op(Op::PUSHI,4u); f.op(Op::PUSHV,h.visual+5); f.op(Op::ADD); f.op(Op::PUSHV,h.visual+5); f.op(Op::MOVI); land(textureDone);
  visualBody(femaleBody); land(done); f.op(Op::RSR);
  const auto mil=constant("GIL_MIL",2),bau=constant("GIL_BAU",8);
  std::array<uint32_t,2> tables{};
  for(size_t table=0;table<tables.size();++table) {
    const char* name=table==0 ? "B_CREATEAMBIENTINV_MIL" : "B_CREATEAMBIENTINV_BAU";
    tables[table]=function(name,{Type::INT},{"INVENTORYSET"}); f.begin(tables[table]); parameters(tables[table]);
    for(uint32_t roll=0;roll<7;++roll) {
      f.op(Op::PUSHI,roll); f.op(Op::PUSHV,tables[table]+1); f.op(Op::EQ); const auto next=jump(Op::BZ);
      f.op(Op::PUSHVI,self); f.op(Op::PUSHI,ambientItem); f.op(Op::PUSHI,roll+1+uint32_t(table)); f.op(Op::BE,create);
      f.op(Op::PUSHVI,self); f.op(Op::PUSHI,trailerItem); f.op(Op::PUSHI,2u); f.op(Op::BE,create); land(next);
      }
    f.op(Op::RSR);
    }
  h.ambient=function("B_CREATEAMBIENTINV",{Type::INSTANCE},{"SLF"});
  const auto draw=f.symbol("B_CREATEAMBIENTINV.ZUFALL",Type::INT);
  f.begin(h.ambient); parameters(h.ambient);
  f.op(Op::PUSHI,7u); f.op(Op::BE,random); f.op(Op::PUSHV,draw); f.op(Op::MOVI);
  f.op(Op::PUSHV,mil); member(h.ambient+1,"C_NPC.GUILD"); f.op(Op::EQ); const auto fallback=jump(Op::BZ);
  f.op(Op::PUSHV,draw); f.call(tables[0]); const auto ambientDone=jump(Op::B); land(fallback);
  f.op(Op::PUSHV,draw); f.call(tables[1]); land(ambientDone); f.op(Op::RSR);
  (void)bau;
  h.skeleton=f.fn("B_SETVISUALS_SKELETON_LORD"); f.begin(h.skeleton);
  f.op(Op::PUSHVI,self); f.op(Op::PUSHV,text("HumanS.mds")); f.op(Op::BE,model);
  for(const auto* name:{"humans_1hST1.mds","humans_2hST2.mds","humans_BowT1.mds","humans_CBowT1.mds"}) {
    f.op(Op::PUSHVI,self); f.op(Op::PUSHV,text(name)); f.op(Op::BE,overlay);
    }
  f.op(Op::PUSHVI,self); f.op(Op::PUSHI,0u); f.op(Op::PUSHV,h.head); f.op(Op::PUSHI,0u); f.op(Op::PUSHI,0u);
  f.op(Op::PUSHI,armor); f.call(h.visual); f.op(Op::RSR);
  constant("NPC_TALENT_MAGE",7);
  h.attributes=function("B_SETATTRIBUTESTOCHAPTER",{Type::INSTANCE,Type::INT},{"SLF","KAP"});
  f.begin(h.attributes); parameters(h.attributes);
  f.op(Op::PUSHVI,h.attributes+1); f.op(Op::PUSHI,7u); f.op(Op::PUSHI,6u); f.op(Op::BE,skill);
  for(uint32_t chapter=0;chapter<=6;++chapter) {
    f.op(Op::PUSHI,chapter); f.op(Op::PUSHV,h.attributes+2); f.op(chapter==6 ? Op::GTE : Op::EQ); const auto next=jump(Op::BZ);
    assign(h.attributes+1,"C_NPC.LEVEL",0,int(chapter*10+3));
    assign(h.attributes+1,"C_NPC.ATTRIBUTE",4,int(chapter*25+10));
    assign(h.attributes+1,"C_NPC.AIVAR",81,int(chapter*25+10)); land(next);
    }
  // Preserve Gothic's integer evaluation order, including truncation before multiplication.
  for(const auto& out:{std::pair{"C_NPC.EXP",1u},std::pair{"C_NPC.EXP_NEXT",2u}}) {
    f.op(Op::PUSHI,1u); member(h.attributes+1,"C_NPC.LEVEL"); f.op(Op::ADD);
    f.op(Op::PUSHI,2u); f.op(Op::PUSHI,out.second); member(h.attributes+1,"C_NPC.LEVEL"); f.op(Op::ADD);
    f.op(Op::DIV); f.op(Op::PUSHI,500u); f.op(Op::MUL); f.op(Op::MUL); member(h.attributes+1,out.first); f.op(Op::MOVI);
    }
  f.op(Op::RSR);
  h.precedingRandom=f.fn("DRAW_BEFORE_AMBIENT"); f.begin(h.precedingRandom);
  f.op(Op::PUSHI,1000u); f.op(Op::BE,random); member(self,"C_NPC.AIVAR",1); f.op(Op::MOVI); f.op(Op::RSR);
  return h;
  }

inline NpcSnapshot::Program program() {
  Fixture f;
  const auto npcClass=f.cls<INpc>("C_NPC"),itemClass=f.cls<IItem>("C_ITEM");
  const auto self=f.symbol("SELF",Type::INSTANCE,1,0,int32_t(npcClass));
  f.symbol("ITEM",Type::INSTANCE,1,0,int32_t(itemClass));
  for(const char* name:{"OTHER","VICTIM","HERO"}) f.symbol(name,Type::INSTANCE,1,0,int32_t(npcClass));
  f.symbol("SPELLFXINSTANCENAMES",Type::STRING);
  f.symbol("SPELLFXANILETTERS",Type::STRING);
  const auto chapter=f.symbol("CHAPTER",Type::INT);
  const auto placementText=f.symbol("PLACEMENT_TEXT",Type::STRING);
  const auto placementLimits=f.symbol("PLACEMENT_LIMITS",Type::INT,2);
  f.p.symbols[placementLimits].int_values[0]=4;
  const auto placementScale=f.symbol("PLACEMENT_SCALE",Type::FLOAT);
  const auto random=f.fn("HLP_RANDOM",{Type::INT},true,Type::INT);
  const auto create=f.fn("CREATEINVITEMS",{Type::INSTANCE,Type::INT,Type::INT},true);
  const auto equip=f.fn("EQUIPITEM",{Type::INSTANCE,Type::INT},true);
  const auto fatness=f.fn("MDL_SETMODELFATNESS",{Type::INSTANCE,Type::FLOAT},true);
  const auto ta=f.fn("TA_MIN",{Type::INSTANCE,Type::INT,Type::INT,Type::INT,Type::INT,Type::INT,Type::STRING},true);
  const auto unknown=f.fn("FUTURE_NPC_ENGINE",{Type::INSTANCE},true);
  const auto text=f.symbol("NPC_NAME",Type::STRING,1,Flag::CONST);
  f.p.symbols[text].string_values[0]=std::string("NPC\0\x80\xff",6);
  const auto callback=f.fn("ON_EQUIP"); f.begin(callback);
  f.op(Op::PUSHV,f.find("C_NPC.ATTRIBUTE")); f.op(Op::PUSHI,5); f.op(Op::ADD);
  f.op(Op::PUSHV,f.find("C_NPC.ATTRIBUTE")); f.op(Op::MOVI);
  f.op(Op::PUSHI,60u); f.op(Op::PUSHVV,f.find("C_NPC.ATTRIBUTE")); f.p.bytecode.push_back(std::byte{4}); f.op(Op::ADDMOVI);
  f.op(Op::PUSHI,10u); f.op(Op::PUSHVV,f.find("C_NPC.HITCHANCE")); f.p.bytecode.push_back(std::byte{1}); f.op(Op::ADDMOVI);
  f.set(f.find("C_NPC.LEVEL"),5); f.op(Op::RSR);
  const auto addAfterEquip=f.fn("ADD_AFTER_EQUIP"); f.begin(addAfterEquip);
  f.op(Op::PUSHV,f.find("C_NPC.ATTRIBUTE")); f.op(Op::PUSHI,15); f.op(Op::ADD);
  f.op(Op::PUSHV,f.find("C_NPC.ATTRIBUTE")); f.op(Op::MOVI);
  f.op(Op::PUSHV,f.find("C_NPC.ATTRIBUTE")); f.op(Op::PUSHV,f.find("C_NPC.EXP")); f.op(Op::MOVI); f.op(Op::RSR);
  const auto item=f.symbol("IT_NPC_RING",Type::INSTANCE,0,Flag::CONST,int32_t(itemClass)); f.begin(item);
  f.set(f.find("C_ITEM.FLAGS"),1<<11);
  f.set(f.find("C_ITEM.VALUE"),7); f.set(f.find("C_ITEM.ON_EQUIP"),int(callback)); f.op(Op::RSR);
  const auto ambientItem=f.symbol("IT_NPC_AMBIENT",Type::INSTANCE,0,Flag::CONST,int32_t(itemClass)); f.begin(ambientItem);
  f.set(f.find("C_ITEM.VALUE"),11); f.op(Op::RSR);
  const auto trailerItem=f.symbol("IT_NPC_TRAILER",Type::INSTANCE,0,Flag::CONST,int32_t(itemClass)); f.begin(trailerItem);
  f.set(f.find("C_ITEM.VALUE"),13); f.op(Op::RSR);
  const auto itemPrototype=f.symbol("IT_NPC_PROTOTYPE",Type::PROTOTYPE,0,Flag::CONST,int32_t(itemClass));
  f.begin(itemPrototype); f.set(f.find("C_ITEM.VALUE"),1); f.op(Op::RSR);
  const auto helper=helpers(f,npcClass,self,random,create,item,ambientItem,trailerItem);
  const auto point=f.symbol("POINT",Type::STRING,1,Flag::CONST); f.p.symbols[point].string_values[0]="TEST_WP";
  const auto state=f.fn("ZS_TEST"); f.begin(state);
  f.op(Op::PUSHI,7); f.op(Op::BE,random); f.op(Op::RSR);
  const auto routine=f.fn("RTN_START_123"); f.begin(routine);
  f.op(Op::PUSHV,self); f.op(Op::PUSHI,8); f.op(Op::PUSHI,0); f.op(Op::PUSHI,20); f.op(Op::PUSHI,0);
  f.op(Op::PUSHI,state); f.op(Op::PUSHV,point); f.op(Op::BE,ta); f.op(Op::RSR);
  const auto prototype=f.symbol("NPC_BASE",Type::PROTOTYPE,0,Flag::CONST,int32_t(npcClass)); f.begin(prototype);
  f.set(f.find("C_NPC.DAILY_ROUTINE"),int(routine));
  f.set(f.find("C_NPC.ATTRIBUTE"),10); f.set(f.find("C_NPC.GUILD"),2);
  f.op(Op::PUSHI,40u); f.op(Op::PUSHVV,f.find("C_NPC.ATTRIBUTE")); f.p.bytecode.push_back(std::byte{4}); f.op(Op::MOVI); f.op(Op::RSR);
  const auto npc=f.symbol("NPC_NO_ID",Type::INSTANCE,0,Flag::CONST,int32_t(prototype)); f.begin(npc);
  f.call(prototype);
  // Untouched AIVAR elements have the same zero value in fresh NPCs and snapshots.
  f.op(Op::PUSHV,f.find("C_NPC.AIVAR")); f.op(Op::PUSHV,f.find("C_NPC.EXP_NEXT")); f.op(Op::MOVI);
  f.op(Op::PUSHV,text); f.op(Op::PUSHV,f.find("C_NPC.NAME")); f.op(Op::MOVS);
  // A string argument must reuse its constant literal rather than alter VM scratch strings.
  f.op(Op::PUSHV,self); f.op(Op::PUSHI,0); f.op(Op::PUSHI,0); f.op(Op::PUSHI,8); f.op(Op::PUSHI,0);
  f.op(Op::PUSHI,state); f.op(Op::PUSHV,point); f.op(Op::BE,ta);
  // CreateInvItems does not allocate an item for a zero amount, even for a prototype symbol.
  f.op(Op::PUSHV,self); f.op(Op::PUSHI,itemPrototype); f.op(Op::PUSHI,0); f.op(Op::BE,create);
  f.op(Op::PUSHV,self); f.op(Op::PUSHI,item); f.op(Op::PUSHI,3); f.op(Op::BE,create);
  f.op(Op::PUSHV,self); f.op(Op::PUSHI,0u); f.op(Op::PUSHV,helper.head); f.op(Op::PUSHI,0u); f.op(Op::PUSHI,0u);
  f.op(Op::PUSHI,item); f.call(helper.visual);
  f.op(Op::PUSHV,self); f.op(Op::PUSHI,30u); f.call(helper.fight);
  // The helper must read the attribute after on_equip added five, then add fifteen.
  f.call(addAfterEquip);
  // Moving the helper to the end reads forty instead of fifteen; EXP preserves its result.
  f.set(f.find("C_NPC.ATTRIBUTE"),40);
  f.set(f.find("C_NPC.LEVEL"),30);
  f.op(Op::PUSHV,self); f.op(Op::PUSHI,std::bit_cast<uint32_t>(1.25f)); f.op(Op::BE,fatness);
  f.call(helper.precedingRandom);
  f.op(Op::PUSHV,self); f.call(helper.ambient);
  // The ambient item must precede this distinct item, and ITEM must end on the trailer.
  f.op(Op::PUSHV,self); f.op(Op::PUSHI,trailerItem); f.op(Op::PUSHI,1); f.op(Op::BE,create); f.op(Op::RSR);
  const auto named=f.symbol("NPC_WITH_ID",Type::INSTANCE,0,Flag::CONST,int32_t(prototype)); f.begin(named);
  f.call(prototype); f.set(f.find("C_NPC.ID"),123);
  f.call(helper.skeleton);
  f.op(Op::PUSHV,self); f.op(Op::PUSHI,3u); f.call(helper.attributes);
  f.op(Op::PUSHV,self); f.op(Op::PUSHI,1u); f.op(Op::PUSHI,60u); f.call(helper.add); f.op(Op::RSR);
  const auto dependent=f.symbol("NPC_GLOBAL",Type::INSTANCE,0,Flag::CONST,int32_t(npcClass)); f.begin(dependent);
  f.op(Op::PUSHV,chapter); f.op(Op::PUSHV,f.find("C_NPC.LEVEL")); f.op(Op::MOVI); f.op(Op::RSR);
  const auto writer=f.symbol("NPC_WRITER",Type::INSTANCE,0,Flag::CONST,int32_t(npcClass)); f.begin(writer);
  f.set(chapter,9); f.op(Op::RSR);
  const auto unsupported=f.symbol("NPC_UNKNOWN",Type::INSTANCE,0,Flag::CONST,int32_t(npcClass)); f.begin(unsupported);
  f.op(Op::PUSHV,self); f.op(Op::BE,unknown); f.op(Op::RSR);
  const auto invalidItem=f.symbol("NPC_BAD_ITEM",Type::INSTANCE,0,Flag::CONST,int32_t(npcClass)); f.begin(invalidItem);
  f.op(Op::PUSHV,self); f.op(Op::PUSHI,itemPrototype); f.op(Op::PUSHI,1); f.op(Op::BE,create); f.op(Op::RSR);
  const auto engineRead=f.symbol("NPC_ENGINE_READ",Type::INSTANCE,0,Flag::CONST,int32_t(prototype)); f.begin(engineRead);
  f.call(prototype);
  f.op(Op::PUSHV,self); f.op(Op::PUSHI,item); f.op(Op::BE,equip);
  // A read in the constructor itself has no helper boundary that can be deferred.
  f.op(Op::PUSHV,f.find("C_NPC.ATTRIBUTE")); f.op(Op::PUSHV,f.find("C_NPC.LEVEL")); f.op(Op::MOVI); f.op(Op::RSR);
  const auto initialRead=f.symbol("NPC_INITIAL_READ",Type::INSTANCE,0,Flag::CONST,int32_t(npcClass)); f.begin(initialRead);
  f.op(Op::PUSHV,f.find("C_NPC.ID")); f.op(Op::PUSHV,f.find("C_NPC.LEVEL")); f.op(Op::MOVI); f.op(Op::RSR);
  const auto insertNpc=f.fn("WLD_INSERTNPC",{Type::INT,Type::STRING},true);
  const auto insertItem=f.fn("WLD_INSERTITEM",{Type::INT,Type::STRING},true);
  const auto guild=f.fn("WLD_SETGUILDATTITUDE",{Type::INT,Type::INT,Type::INT},true);
  const auto mob=f.fn("WLD_SETMOBROUTINE",{Type::INT,Type::INT,Type::STRING,Type::INT},true);
  const auto nested=f.fn("STARTUP_TEST_PART_1"); f.begin(nested);
  f.op(Op::PUSHI,npc); f.op(Op::PUSHV,point); f.op(Op::BE,insertNpc);
  f.op(Op::PUSHI,item); f.op(Op::PUSHV,point); f.op(Op::BE,insertItem); f.op(Op::RSR);
  const auto dependentPlacement=f.fn("PLACE_DEPENDENT"); f.begin(dependentPlacement);
  f.op(Op::PUSHI,1); f.op(Op::PUSHV,chapter); f.op(Op::PUSHI,2); f.op(Op::BE,guild); f.op(Op::RSR);
  const auto startup=f.fn("STARTUP_TEST"); f.begin(startup);
  f.call(nested); f.call(dependentPlacement);
  // Reordering the deferred helper changes the final guild as well as the trace.
  f.op(Op::PUSHI,1); f.op(Op::PUSHI,2); f.op(Op::PUSHI,2); f.op(Op::BE,guild);
  f.op(Op::PUSHI,8); f.op(Op::PUSHI,15); f.op(Op::PUSHV,point); f.op(Op::PUSHI,1); f.op(Op::BE,mob);
  f.op(Op::PUSHI,trailerItem); f.op(Op::PUSHV,point); f.op(Op::BE,insertItem); f.op(Op::RSR);
  const auto error=f.fn("PLACE_SCRIPT_ERROR"); f.begin(error);
  f.op(Op::PUSHI,0); f.op(Op::PUSHI,1); f.op(Op::DIV); f.op(Op::RSR);
  const auto startupError=f.fn("STARTUP_ERROR_HELPER"); f.begin(startupError);
  f.op(Op::PUSHI,item); f.op(Op::PUSHV,point); f.op(Op::BE,insertItem);
  f.call(error);
  f.op(Op::PUSHI,trailerItem); f.op(Op::PUSHV,point); f.op(Op::BE,insertItem); f.op(Op::RSR);
  const auto german=f.fn("GAME_INITGERMAN",{},true,Type::INT);
  const auto constants=f.fn("PLACE_CONSTANTS"); f.begin(constants);
  f.op(Op::BE,german);
  f.set(chapter,9);
  f.op(Op::PUSHV,text); f.op(Op::PUSHV,placementText); f.op(Op::MOVS);
  f.op(Op::PUSHI,27); f.op(Op::PUSHVV,placementLimits); f.p.bytecode.push_back(std::byte{1}); f.op(Op::MOVI);
  f.op(Op::PUSHI,std::bit_cast<uint32_t>(1.25f)); f.op(Op::PUSHV,placementScale); f.op(Op::MOVF);
  f.op(Op::RSR);
  const auto startupGlobal=f.fn("STARTUP_MUTABLE_ROOT"); f.begin(startupGlobal);
  f.call(constants); f.call(dependentPlacement); f.op(Op::RSR);
  const auto startupState=f.fn("STARTUP_STATE_ROOT"); f.begin(startupState);
  f.op(Op::PUSHV,chapter); f.op(Op::PUSHI,1); f.op(Op::ADD); f.op(Op::PUSHV,chapter); f.op(Op::MOVI); f.op(Op::RSR);
  const auto startupQuery=f.fn("STARTUP_QUERY_ROOT"); f.begin(startupQuery);
  f.op(Op::BE,german); f.op(Op::PUSHV,chapter); f.op(Op::MOVI); f.op(Op::RSR);
  const auto startupErrorRoot=f.fn("STARTUP_ERROR_ROOT"); f.begin(startupErrorRoot);
  f.op(Op::PUSHI,0); f.op(Op::PUSHI,1); f.op(Op::DIV); f.op(Op::RSR);
  const auto globalStartup=f.fn("STARTUP_GLOBAL"); f.begin(globalStartup);
  // The real STARTUP_GLOBAL discards this external's return at the void function boundary.
  f.op(Op::BE,german); f.op(Op::RSR);
  activities(f,self,state,ta,point);
  const auto guildClass=f.symbol("C_GILVALUES",Type::CLASS);
  f.p.symbols[guildClass].class_size=sizeof(IGuildValues);
  for(const char* field:{"WATER_DEPTH_KNEE","WATER_DEPTH_CHEST","JUMPUP_HEIGHT","SWIM_TIME","DIVE_TIME",
      "STEP_HEIGHT","JUMPLOW_HEIGHT","JUMPMID_HEIGHT","SLIDE_ANGLE","SLIDE_ANGLE2","DISABLE_AUTOROLL",
      "SURFACE_ALIGN","CLIMB_HEADING_ANGLE","CLIMB_HORIZ_ANGLE","CLIMB_GROUND_ANGLE","FIGHT_RANGE_BASE",
      "FIGHT_RANGE_FIST","FIGHT_RANGE_G","FIGHT_RANGE_1HS","FIGHT_RANGE_1HA","FIGHT_RANGE_2HS","FIGHT_RANGE_2HA",
      "FALLDOWN_HEIGHT","FALLDOWN_DAMAGE","BLOOD_DISABLED","BLOOD_MAX_DISTANCE","BLOOD_AMOUNT","BLOOD_FLOW",
      "BLOOD_EMITTER","BLOOD_TEXTURE","TURN_SPEED"}) {
    const bool text=std::string_view(field)=="BLOOD_EMITTER" || std::string_view(field)=="BLOOD_TEXTURE";
    f.symbol(std::string("C_GILVALUES.")+field,text ? Type::STRING : Type::INT,66,Flag::MEMBER,int32_t(guildClass));
    }
  const auto guildMax=f.symbol("GIL_MAX",Type::INT,1,Flag::CONST);
  f.p.symbols[guildMax].int_values[0]=66;
  const auto guildValues=f.symbol("GIL_VALUES",Type::INSTANCE,0,Flag::CONST,int32_t(guildClass));
  f.begin(guildValues); f.op(Op::RSR);
  const auto encoded=ProgramData::encodeProgram(f.p);
  return {std::move(f.p),encoded.name,encoded.sha256};
  }

}

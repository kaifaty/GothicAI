#pragma once

#include "npcsnapshot.h"
#include "daedalus/classes.h"
#include "daedalus/vm.h"
#include "npcsnapshotfields.h"
#include "programdata.h"

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
// signatures. ZS_SMITH_ANVIL is accepted, ZS_SMITH_FIRE is captured but not accepted, ZS_SMITH_SWAPPED is refused.
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
  auto mobState=[&](const std::string& name,const char* scheme,bool walkFirst=false) {
    const auto init=f.fn(name.c_str()); f.begin(init);
    f.call(perceptions);
    if(walkFirst) { f.op(Op::PUSHVI,self); f.op(Op::PUSHV,walk); f.op(Op::BE,setWalk); }
    f.op(Op::PUSHVI,self); f.call(reset);
    if(!walkFirst) { f.op(Op::PUSHVI,self); f.op(Op::PUSHV,walk); f.op(Op::BE,setWalk); }
    f.op(Op::PUSHV,no); f.op(Op::PUSHVI,self); f.op(Op::BE,nearest);
    f.op(Op::GMOVI,self); f.op(Op::PUSHV,wp); f.op(Op::BE,compare); f.op(Op::EQ);
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
  const auto fire=mobState("ZS_SMITH_FIRE","BSFIRE");
  const auto swapped=mobState("ZS_SMITH_SWAPPED","BSCOOL",true);
  const auto routine=f.fn("RTN_SMITH_7"); f.begin(routine);
  for(auto [from,to,state]:{std::tuple{8u,12u,anvil},std::tuple{12u,16u,fire},std::tuple{16u,20u,swapped}}) {
    f.op(Op::PUSHVI,self); f.op(Op::PUSHI,from); f.op(Op::PUSHI,0u); f.op(Op::PUSHI,to); f.op(Op::PUSHI,0u);
    f.op(Op::PUSHI,state); f.op(Op::PUSHV,point); f.op(Op::BE,ta);
    }
  f.op(Op::RSR);
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
  const auto ambient=f.fn("B_CREATEAMBIENTINV",{Type::INSTANCE});
  const auto count=f.symbol("B_CREATEAMBIENTINV.COUNT",Type::INT);
  f.begin(ambient); f.op(Op::PUSHV,ambient+1); f.op(Op::MOVVI);
  f.op(Op::PUSHI,7); f.op(Op::BE,random); f.op(Op::PUSHI,1); f.op(Op::ADD);
  f.op(Op::PUSHV,count); f.op(Op::MOVI);
  f.op(Op::PUSHV,ambient+1); f.op(Op::PUSHI,ambientItem); f.op(Op::PUSHV,count); f.op(Op::BE,create); f.op(Op::RSR);
  const auto point=f.symbol("POINT",Type::STRING,1,Flag::CONST); f.p.symbols[point].string_values[0]="TEST_WP";
  const auto state=f.fn("ZS_TEST"); f.begin(state);
  f.op(Op::PUSHI,7); f.op(Op::BE,random); f.op(Op::RSR);
  const auto routine=f.fn("RTN_START_123"); f.begin(routine);
  f.op(Op::PUSHV,self); f.op(Op::PUSHI,8); f.op(Op::PUSHI,0); f.op(Op::PUSHI,20); f.op(Op::PUSHI,0);
  f.op(Op::PUSHI,state); f.op(Op::PUSHV,point); f.op(Op::BE,ta); f.op(Op::RSR);
  const auto prototype=f.symbol("NPC_BASE",Type::PROTOTYPE,0,Flag::CONST,int32_t(npcClass)); f.begin(prototype);
  f.set(f.find("C_NPC.DAILY_ROUTINE"),int(routine));
  f.set(f.find("C_NPC.ATTRIBUTE"),10); f.set(f.find("C_NPC.GUILD"),2); f.op(Op::RSR);
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
  f.op(Op::PUSHV,self); f.op(Op::PUSHI,item); f.op(Op::BE,equip);
  // The helper must read the attribute after on_equip added five, then add fifteen.
  f.call(addAfterEquip);
  // Moving the helper to the end reads forty instead of fifteen; EXP preserves its result.
  f.set(f.find("C_NPC.ATTRIBUTE"),40);
  f.set(f.find("C_NPC.LEVEL"),30);
  f.op(Op::PUSHV,self); f.op(Op::PUSHI,std::bit_cast<uint32_t>(1.25f)); f.op(Op::BE,fatness);
  f.op(Op::PUSHV,self); f.call(ambient);
  // The ambient item must precede this distinct item, and ITEM must end on the trailer.
  f.op(Op::PUSHV,self); f.op(Op::PUSHI,trailerItem); f.op(Op::PUSHI,1); f.op(Op::BE,create); f.op(Op::RSR);
  const auto named=f.symbol("NPC_WITH_ID",Type::INSTANCE,0,Flag::CONST,int32_t(prototype)); f.begin(named);
  f.call(prototype); f.set(f.find("C_NPC.ID"),123); f.op(Op::RSR);
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

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
inline NpcSnapshot::Program program() {
  Fixture f;
  const auto npcClass=f.cls<INpc>("C_NPC"),itemClass=f.cls<IItem>("C_ITEM");
  const auto self=f.symbol("SELF",Type::INSTANCE,1,0,int32_t(npcClass));
  f.symbol("ITEM",Type::INSTANCE,1,0,int32_t(itemClass));
  for(const char* name:{"OTHER","VICTIM","HERO"}) f.symbol(name,Type::INSTANCE,1,0,int32_t(npcClass));
  f.symbol("SPELLFXINSTANCENAMES",Type::STRING);
  f.symbol("SPELLFXANILETTERS",Type::STRING);
  const auto chapter=f.symbol("CHAPTER",Type::INT);
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
  f.op(Op::PUSHV,f.find("C_NPC.ATTRIBUTE")); f.op(Op::MOVI); f.op(Op::RSR);
  const auto item=f.symbol("IT_NPC_RING",Type::INSTANCE,0,Flag::CONST,int32_t(itemClass)); f.begin(item);
  f.set(f.find("C_ITEM.FLAGS"),1<<11);
  f.set(f.find("C_ITEM.VALUE"),7); f.set(f.find("C_ITEM.ON_EQUIP"),int(callback)); f.op(Op::RSR);
  const auto itemPrototype=f.symbol("IT_NPC_PROTOTYPE",Type::PROTOTYPE,0,Flag::CONST,int32_t(itemClass));
  f.begin(itemPrototype); f.set(f.find("C_ITEM.VALUE"),1); f.op(Op::RSR);
  const auto ambient=f.fn("B_CREATEAMBIENTINV",{Type::INSTANCE});
  const auto count=f.symbol("B_CREATEAMBIENTINV.COUNT",Type::INT);
  f.begin(ambient); f.op(Op::PUSHV,ambient+1); f.op(Op::MOVVI);
  f.op(Op::PUSHI,7); f.op(Op::BE,random); f.op(Op::PUSHI,1); f.op(Op::ADD);
  f.op(Op::PUSHV,count); f.op(Op::MOVI);
  f.op(Op::PUSHV,ambient+1); f.op(Op::PUSHI,item); f.op(Op::PUSHV,count); f.op(Op::BE,create); f.op(Op::RSR);
  const auto point=f.symbol("POINT",Type::STRING,1,Flag::CONST); f.p.symbols[point].string_values[0]="TEST_WP";
  const auto state=f.fn("ZS_TEST"); f.begin(state); f.op(Op::RSR);
  const auto routine=f.fn("RTN_START_123"); f.begin(routine);
  f.op(Op::PUSHV,self); f.op(Op::PUSHI,8); f.op(Op::PUSHI,0); f.op(Op::PUSHI,20); f.op(Op::PUSHI,0);
  f.op(Op::PUSHI,state); f.op(Op::PUSHV,point); f.op(Op::BE,ta); f.op(Op::RSR);
  const auto prototype=f.symbol("NPC_BASE",Type::PROTOTYPE,0,Flag::CONST,int32_t(npcClass)); f.begin(prototype);
  f.set(f.find("C_NPC.DAILY_ROUTINE"),int(routine));
  f.set(f.find("C_NPC.ATTRIBUTE"),10); f.set(f.find("C_NPC.GUILD"),2); f.op(Op::RSR);
  const auto npc=f.symbol("NPC_NO_ID",Type::INSTANCE,0,Flag::CONST,int32_t(prototype)); f.begin(npc);
  f.call(prototype);
  f.op(Op::PUSHV,text); f.op(Op::PUSHV,f.find("C_NPC.NAME")); f.op(Op::MOVS);
  // A string argument must reuse its constant literal rather than alter VM scratch strings.
  f.op(Op::PUSHV,self); f.op(Op::PUSHI,0); f.op(Op::PUSHI,0); f.op(Op::PUSHI,8); f.op(Op::PUSHI,0);
  f.op(Op::PUSHI,state); f.op(Op::PUSHV,point); f.op(Op::BE,ta);
  // CreateInvItems does not allocate an item for a zero amount, even for a prototype symbol.
  f.op(Op::PUSHV,self); f.op(Op::PUSHI,itemPrototype); f.op(Op::PUSHI,0); f.op(Op::BE,create);
  f.op(Op::PUSHV,self); f.op(Op::PUSHI,item); f.op(Op::PUSHI,3); f.op(Op::BE,create);
  f.op(Op::PUSHV,self); f.op(Op::PUSHI,item); f.op(Op::BE,equip);
  // on_equip changed attribute[0]; this later assignment must occur after that call.
  f.set(f.find("C_NPC.ATTRIBUTE"),30);
  f.op(Op::PUSHV,self); f.op(Op::PUSHI,std::bit_cast<uint32_t>(1.25f)); f.op(Op::BE,fatness);
  f.op(Op::PUSHV,self); f.call(ambient); f.op(Op::RSR);
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
  const auto encoded=ProgramData::encodeProgram(f.p);
  return {std::move(f.p),encoded.name,encoded.sha256};
  }

}

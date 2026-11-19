#include "npchelperdefinitions.h"

#include <bit>
#include <stdexcept>
#include <type_traits>

namespace {
using namespace NpcHelperDefinitions;
using Kind = Value::Kind;
Value integer(int32_t value) { Value result; result.integer=value; return result; }
Value text(std::string_view value) { Value result; result.kind=Kind::String; result.text=value; return result; }
Value symbol(std::string_view value) { Value result; result.kind=Kind::Symbol; result.text=value; return result; }
Value real(float value) { Value result; result.kind=Kind::Float; result.real=value; return result; }
Value self() { Value result; result.kind=Kind::Self; return result; }
int32_t add(int32_t a,int32_t b) { return std::bit_cast<int32_t>(uint32_t(a)+uint32_t(b)); }
int32_t multiply(int32_t a,int32_t b) { return std::bit_cast<int32_t>(uint32_t(a)*uint32_t(b)); }
void call(Host& host,std::string_view name,std::initializer_list<Value> arguments) {
  host.external(name,std::span<const Value>(arguments.begin(),arguments.size()));
  }
void parameters(Host& host,std::string_view name,int32_t talent,int32_t percent,std::string_view npc) {
  host.local(std::string(name)+".PERCENT",integer(percent));
  host.local(std::string(name)+".TALENT",integer(talent));
  host.local(std::string(name)+"."+std::string(npc),self());
  }
void addFightSkill(const AddFightSkill& definition,int32_t talent,int32_t percent,Host& host) {
  parameters(host,"B_ADDFIGHTSKILL",talent,percent,"SLF");
  // These are separate conditions, as in the source: every matching stage calls the engine.
  for(const auto& entry:definition.talents) {
    if(talent!=entry.talent) continue;
    host.set("C_NPC.HITCHANCE",uint16_t(entry.chance),add(host.get("C_NPC.HITCHANCE",uint16_t(entry.chance)),percent));
    for(const auto& stage:entry.thresholds)
      if(host.get("C_NPC.HITCHANCE",uint16_t(stage.chance))>=stage.threshold)
        call(host,"NPC_SETTALENTSKILL",{self(),integer(entry.talent),integer(stage.skill)});
    }
  }
void setFightSkills(const SetFightSkills& definition,int32_t percent,Host& host) {
  host.local("B_SETFIGHTSKILLS.PERCENT",integer(percent));
  host.local("B_SETFIGHTSKILLS.SLF",self());
  for(const auto& entry:definition.fight.talents) {
    parameters(host,"B_RAISEFIGHTTALENT",entry.talent,percent,"OTH");
    parameters(host,"B_RAISEREALFIGHTTALENTPERCENT",entry.talent,percent,"OTH");
    host.set("C_NPC.AIVAR",uint16_t(entry.aivar),add(host.get("C_NPC.AIVAR",uint16_t(entry.aivar)),percent));
    addFightSkill(definition.fight,entry.talent,percent,host);
    }
  }
void setNpcVisual(const SetNpcVisual& definition,int32_t gender,const Value& head,int32_t face,
                  int32_t body,const Value& armor,Host& host) {
  host.local("B_SETNPCVISUAL.ARMORINSTANCE",armor);
  host.local("B_SETNPCVISUAL.BODYTEX",integer(body));
  host.local("B_SETNPCVISUAL.FACETEX",integer(face));
  host.local("B_SETNPCVISUAL.HEADMESH",head);
  host.local("B_SETNPCVISUAL.GENDER",integer(gender));
  host.local("B_SETNPCVISUAL.SLF",self());
  host.set("C_NPC.AIVAR",uint16_t(definition.genderAivar),gender);
  call(host,"MDL_SETVISUAL",{self(),text(definition.model)});
  auto visualBody=[&](const std::string& mesh) {
    call(host,"MDL_SETVISUALBODY",{self(),text(mesh),integer(body),integer(definition.bodyColor),
                                  head,integer(face),integer(definition.teeth),armor});
    };
  if(gender==definition.male) {
    visualBody(definition.maleBody);
    // Body/armor creation and on_equip have already run when strength is read here.
    if(host.get("C_NPC.ATTRIBUTE",uint16_t(definition.strength))<definition.thinLimit)
      call(host,"MDL_SETMODELSCALE",{self(),real(definition.thinScale[0]),real(definition.thinScale[1]),real(definition.thinScale[2])});
    if(host.get("C_NPC.ATTRIBUTE",uint16_t(definition.strength))>definition.strongLimit)
      call(host,"MDL_SETMODELSCALE",{self(),real(definition.strongScale[0]),real(definition.strongScale[1]),real(definition.strongScale[2])});
    }
  else {
    if(body>=definition.femaleTextureFirst && body<=definition.femaleTextureLast) {
      body=add(body,definition.femaleTextureOffset);
      host.local("B_SETNPCVISUAL.BODYTEX",integer(body));
      }
    visualBody(definition.femaleBody);
    }
  }
void ambientInventory(const AmbientInventory& definition,Host& host) {
  host.local("B_CREATEAMBIENTINV.SLF",self());
  const auto roll=host.random(definition.randomBound);
  host.local("B_CREATEAMBIENTINV.ZUFALL",integer(roll));
  const auto found=definition.guilds.find(host.get("C_NPC.GUILD"));
  const auto& table=found==definition.guilds.end() ? definition.fallback : found->second;
  host.local(table.helper+".INVENTORYSET",integer(roll));
  for(const auto& item:table.rolls.at(size_t(roll)))
    call(host,"CREATEINVITEMS",{self(),symbol(item.item),integer(item.count)});
  }
void skeletonLord(const SkeletonLord& definition,Host& host) {
  call(host,"MDL_SETVISUAL",{self(),text(definition.model)});
  for(const auto& overlay:definition.overlays)
    call(host,"MDL_APPLYOVERLAYMDS",{self(),text(overlay)});
  setNpcVisual(definition.visual,definition.gender,text(definition.head),definition.faceTexture,
               definition.bodyTexture,symbol(definition.armor),host);
  }
void attributesToChapter(const AttributesToChapter& definition,int32_t chapter,Host& host) {
  host.local("B_SETATTRIBUTESTOCHAPTER.KAP",integer(chapter));
  host.local("B_SETATTRIBUTESTOCHAPTER.SLF",self());
  call(host,"NPC_SETTALENTSKILL",{self(),integer(definition.talent),integer(definition.skill)});
  for(const auto& entry:definition.chapters)
    if(entry.minimum ? chapter>=entry.chapter : chapter==entry.chapter)
      for(const auto& field:entry.fields)
        host.set(field.binding,field.index,field.value.integer);
  const auto level=host.get("C_NPC.LEVEL");
  const auto next=add(level,1);
  host.set("C_NPC.EXP",0,multiply(multiply(next/definition.experienceDivisor,definition.experienceMultiplier),next));
  host.set("C_NPC.EXP_NEXT",0,multiply(multiply(add(level,2)/definition.experienceDivisor,definition.experienceMultiplier),next));
  }
}

void NpcHelperDefinitions::run(const Definition& definition,std::span<const Value> args,Host& host) {
  std::visit([&](const auto& helper) {
    using T=std::decay_t<decltype(helper)>;
    if constexpr(std::is_same_v<T,SetFightSkills>) setFightSkills(helper,args[1].integer,host);
    else if constexpr(std::is_same_v<T,SetNpcVisual>)
      setNpcVisual(helper,args[1].integer,args[2],args[3].integer,args[4].integer,args[5],host);
    else if constexpr(std::is_same_v<T,AmbientInventory>) ambientInventory(helper,host);
    else if constexpr(std::is_same_v<T,AddFightSkill>) addFightSkill(helper,args[1].integer,args[2].integer,host);
    else if constexpr(std::is_same_v<T,SkeletonLord>) skeletonLord(helper,host);
    else if constexpr(std::is_same_v<T,AttributesToChapter>) attributesToChapter(helper,args[1].integer,host);
    },definition);
  }

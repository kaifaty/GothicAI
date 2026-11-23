#include "npc_definition_fixture.h"
#include "npc_world_contract_access.h"
#include "npcdayjournal.h"
#include "npcdaymeasurement.h"
#include "storygraph/eval.h"

#include "baseidentity.h"
#include "baseimport.h"
#include "commandline.h"
#include "game/definitions/basescripts.h"
#include "game/gamesession.h"
#include "gothic.h"
#include "gamemusic.h"
#include "programdata.h"
#include "resources.h"
#include "world/activityevents.h"
#include "world/objects/npc.h"
#include "world/world.h"
#include "worldchunks.h"
#include "worlddata.h"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;

void write(const fs::path& file, const std::string& bytes) {
  fs::create_directories(file.parent_path());
  std::ofstream out(file,std::ios::binary);
  out.write(bytes.data(),std::streamsize(bytes.size()));
  if(!out) throw std::runtime_error("cannot write synthetic NPC fixture: "+file.string());
  }

struct Environment {
  std::vector<std::pair<std::string,std::optional<std::string>>> saved;
  void set(const char* name, const std::optional<std::string>& value) {
    const auto* previous=std::getenv(name);
    saved.emplace_back(name,previous ? std::optional<std::string>(previous) : std::nullopt);
    if(value) setenv(name,value->c_str(),1);
    else unsetenv(name);
    }
  ~Environment() {
    for(auto it=saved.rbegin();it!=saved.rend();++it) {
      if(it->second) setenv(it->first.c_str(),it->second->c_str(),1);
      else unsetenv(it->first.c_str());
      }
    }
  };

void emptyIndices(const fs::path& base) {
  for(const auto* kind:{"textures","meshes","zen","models","fonts"}) {
    const auto entries=std::string_view(kind)=="models" || std::string_view(kind)=="zen" ? "files" : kind;
    write(base/kind/(std::string(kind)+".json"),
          "{\"format\":\"gothic-"+std::string(kind)+"\",\"version\":1,\"archives\":[],\"loose_files\":[],\""+entries+"\":{}}\n");
    }
  write(base/"audio/audio.json",
        "{\"format\":\"gothic-audio\",\"version\":1,\"archives\":[],\"loose_files\":[],\"payloads\":{},\"sounds\":{}}\n");
  write(base/"music/music.json",
        "{\"format\":\"gothic-music\",\"version\":1,\"archives\":[],\"loose_files\":[],\"records\":{},\"sources\":[],\"gk\":{},\"dx8\":{}}\n");
  write(base/"outputunits/outputunits.json",
        "{\"format\":\"gothic-outputunits\",\"version\":1,\"archives\":[],\"loose_files\":[],\"records\":{},\"sources\":[],\"vfs\":{},\"host\":{},\"selections\":[],\"server\":{\"ou_py\":null}}\n");
  write(base/"sources/sources.json","{\"format\":\"gothic-sources\",\"version\":1}\n");
  }

void world(const fs::path& base) {
  const auto dir=base/"worlds/NPC_CONTRACT";
  const WorldChunkSource source{"NPC_CONTRACT.ZEN",1,0};
  writeWorldChunks(asset::WorldMesh{},source,dir);
  // Runtime world selection also binds the archive membership of the source installation.
  const auto path=dir/"world.json";
  std::ifstream input(path);
  std::string json{std::istreambuf_iterator<char>(input),{}};
  json.insert(json.find('{')+1,"\"archives\":[],");
  write(path,json);
  WorldData::Data data;
  data.source=source;
  data.name="NPC_CONTRACT";
  data.waynet=std::make_shared<asset::WayNet>();
  auto point=std::make_shared<asset::WayPoint>();
  point->name="TEST_WP";
  point->position={0,0,0};
  point->direction={0,0,1};
  point->water_depth=0;
  point->under_water=false;
  data.waynet->points.push_back(point);
  WorldData::write(data,dir);
  }

fs::path program(const fs::path& base) {
  const auto value=NpcDefinitionFixture::program();
  for(const char* name:{"TEST","ERROR_HELPER","MUTABLE_ROOT","STATE_ROOT","QUERY_ROOT","ERROR_ROOT"}) fs::create_directories(base/"worlds"/name);
  fs::create_directories(base/"programs");
  std::string name;
  const auto sha=ProgramData::writeProgram(value.data,base/"programs",&name);
  const auto source="{\"origin\":\"host\",\"path\":\"system/GOTHIC.DAT\",\"archive\":null,\"size\":1,\"sha256\":\""+
                    std::string(64,'0')+"\",\"program\":\""+name+"\",\"trailing_bytes\":0,\"skipped\":null}";
  write(base/"programs/programs.json",
        "{\"format\":\"gothic-programs\",\"version\":1,\"archives\":[],\"loose_files\":[],\"programs\":{\""+name+"\":\""+sha+
        "\"},\"sources\":["+source+"],\"vfs\":{\"GOTHIC.DAT\":0,\"FIGHT.DAT\":0,\"CAMERA.DAT\":0,\"SFX.DAT\":0,\"PARTICLEFX.DAT\":0,\"VISUALFX.DAT\":0,\"MUSIC.DAT\":0},\"host\":{},\"selections\":[{\"request\":\"GOTHIC.DAT\",\"language\":-1,\"step\":\"vfs_find_request\",\"path\":\"GOTHIC.DAT\",\"source\":0}],\"server\":{\"storygraph_dat\":0}}\n");
  const auto definitions=base/"definitions"/sha;
  BaseImport::convertFightDefinitions(base);
  NpcSnapshot::writeItems(value,definitions/"items.json");
  NpcSnapshot::writeRoutines(value,definitions/"routines.json");
  NpcSnapshot::writeNpcs(value,definitions/"npcs.json");
  NpcSnapshot::writeHelpers(value,definitions/"helpers.json",definitions/"npcs.json");
  NpcSnapshot::writePlacements(value,definitions/"placement.json",base/"worlds");
  NpcSnapshot::writeActivities(value,definitions/"activities.json",definitions/"routines.json");
  return definitions/"npcs.json";
  }

void npcDayRandom(World& world) {
  const auto directory=std::filesystem::temp_directory_path()/("gothic-npc-day-live-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(directory);
  const auto check=[](bool value,const char* message) { if(!value) throw std::runtime_error(message); };
  auto& script=world.script();
  auto* npc=world.addNpc(script.findSymbolIndex("NPC_NO_ID"),Tempest::Vec3{});
  check(npc!=nullptr,"day trace NPC must exist");
  auto& vm=script.getVm();
  const auto state=vm.find_symbol_by_name("ZS_TEST")->index();
  const auto observers=vm.call_observer_count();
  std::string error;
  NpcDayJournal::Options options;
  options.directory=directory.string();
  options.isolate=false;
  options.scriptProbe=true;
  options.traceNpcs={"NPC_NO_ID"};
  if(!NpcDayMeasurement::start(world,options,error)) throw std::runtime_error(error);
  check(vm.call_observer_count()==observers+1,"starting a day measurement must install its call observer");
  script.invokeState(npc,nullptr,nullptr,state);
  npc->aiPush(AiQueue::aiUseMob("SMITH_ANVIL",1));
  npc->aiPush(AiQueue::aiOutputSvmOverlay(*npc,"TEST_OVERLAY",7));
  NpcDayMeasurement::stop(world);
  check(vm.call_observer_count()==observers,"stopping a day measurement must remove its call observer");
  std::ifstream file(directory/"npc-day-script.json");
  const std::string bytes{std::istreambuf_iterator<char>(file),{}};
  rapidjson::Document document;
  document.Parse(bytes.c_str());
  check(!document.HasParseError(),"live day counters must be valid JSON");
  const auto& stats=document["stats"];
  const auto& phase=stats["states"]["ZS_TEST"]["phases"]["init"];
  check(phase["engine"].GetUint64()==1 && phase["random"].GetUint64()==1,
          "invokeState must attribute the real Hlp_Random draw to its phase");
  const auto& sources=stats["random_sources"];
  check(stats["random"].GetUint64()==1 && sources.HasMember("HLP_RANDOM/ZS_TEST/init") &&
          sources["HLP_RANDOM/ZS_TEST/init"].GetUint64()==1,
          "GameScript random origin must reach the measurement probe");
  std::ifstream trace(directory/"npc-day-activity.jsonl");
  unsigned commands=0;
  for(std::string line;std::getline(trace,line);) {
    document.Parse(line.c_str());
    check(!document.HasParseError(),"live day activity must be valid JSONL");
    if(std::string_view(document["kind"].GetString())!="queue") continue;
    check(document["npc"].GetUint()==world.npcId(npc) && document["symbol"]=="NPC_NO_ID",
          "queued command must retain its actor");
    if(commands==0)
      check(document["action"].GetInt()==AI_UseMob && document["action_name"]=="UseMob" &&
            document["s0"]=="SMITH_ANVIL" && document["i0"]==1,"aiPush must write the mob command to JSONL");
    else
      check(document["action"].GetInt()==AI_OutputSvmOverlay && document["action_name"]=="OutputSvmOverlay" &&
            document["s0"]=="TEST_OVERLAY" && document["i0"]==7 && document["target"]=="NPC_NO_ID",
            "aiPush must write the overlay command to JSONL");
    ++commands;
    }
  check(commands==2,"both regular and overlay aiPush commands must reach JSONL");
  options.traceNpcs.clear();
  if(!NpcDayMeasurement::start(world,options,error)) throw std::runtime_error(error);
  check(vm.call_observer_count()==observers+1,"starting the second interval must install one call observer");
  script.invokeState(npc,nullptr,nullptr,state);
  NpcDayMeasurement::stop(world);
  check(vm.call_observer_count()==observers,"stopping the second interval must restore the original observer count");
  world.removeNpc(*npc);
  std::filesystem::remove_all(directory);
  std::puts("Live NPC day random: invokeState, VM Hlp_Random, engine rand and measurement attributed one draw");
  std::puts("Live NPC day queue: regular and overlay aiPush commands reached JSONL with their arguments");
  }

void mutations(const fs::path& file, Gothic& gothic, void (*compare)(World&)) {
  std::ifstream input(file);
  const std::string original{std::istreambuf_iterator<char>(input),{}};
  struct Restore {
    fs::path file;
    const std::string& bytes;
    ~Restore() { write(file,bytes); }
    } restore{file,original};
  for(unsigned mutation=0;mutation<7;++mutation) {
    rapidjson::Document document;
    document.Parse(original.data(),original.size());
    if(document.HasParseError()) throw std::runtime_error("invalid synthetic NPC definitions JSON");
    rapidjson::Value* target=nullptr;
    for(auto& record:document["records"].GetArray())
      if(std::string_view(record["symbol"].GetString())=="NPC_NO_ID") target=&record["steps"];
    if(!target) throw std::runtime_error("synthetic NPC mutation target missing");
    auto& steps=*target;
    size_t attribute=steps.Size(),create=steps.Size(),equip=steps.Size(),fatness=steps.Size(),deferred=steps.Size(),preceding=steps.Size();
    for(rapidjson::SizeType i=0;i<steps.Size();++i) {
      const auto& step=steps[i];
      if(step.HasMember("field") && std::string_view(step["field"].GetString())=="C_NPC.ATTRIBUTE" &&
         step["index"].GetUint()==0 && step["value"]["value"].GetInt()==10) attribute=i;
      if(!step.HasMember("function")) continue;
      const auto function=std::string_view(step["function"].GetString());
      if(function=="CREATEINVITEMS" && step["arguments"][1].HasMember("value") &&
         std::string_view(step["arguments"][1]["value"].GetString())=="IT_NPC_RING") create=i;
      if(function=="B_SETNPCVISUAL") equip=i;
      if(function=="DRAW_BEFORE_AMBIENT") preceding=i;
      if(function=="MDL_SETMODELFATNESS") fatness=i;
      if(function=="B_CREATEAMBIENTINV") deferred=i;
      }
    if(attribute>=steps.Size() || create>=steps.Size() || equip>=steps.Size() || fatness>=steps.Size() || deferred>=steps.Size() || preceding>=steps.Size())
      throw std::runtime_error("synthetic NPC mutation steps missing");
    const auto index=[](size_t value) { return rapidjson::SizeType(value); };
    switch(mutation) {
      case 0: steps[index(attribute)]["value"]["value"].SetInt(31); break;
      case 1: steps[index(create)].Swap(steps[index(equip)]); break;
      case 2: steps.Erase(steps.Begin()+ptrdiff_t(fatness)); break;
      case 3: {
        rapidjson::Value field;
        field.CopyFrom(steps[index(attribute)],document.GetAllocator());
        field["field"].SetString("C_NPC.ID",document.GetAllocator());
        field["value"]["value"].SetInt(0);
        steps.PushBack(field,document.GetAllocator());
        break;
        }
      case 4: steps.Erase(steps.Begin()+ptrdiff_t(deferred)); break;
      case 6: steps[index(deferred)].Swap(steps[index(preceding)]); break;
      case 5: {
        rapidjson::Value call;
        call.CopyFrom(steps[index(deferred)],document.GetAllocator());
        steps.PushBack(call,document.GetAllocator());
        break;
        }
      }
    rapidjson::StringBuffer bytes;
    rapidjson::Writer<rapidjson::StringBuffer> writer(bytes);
    if(!document.Accept(writer)) throw std::runtime_error("cannot encode synthetic NPC mutation");
    write(file,{bytes.GetString(),bytes.GetSize()});
    gothic.clearGame().reset();
    gothic.setGame(std::make_unique<GameSession>("NPC_CONTRACT.ZEN"));
    std::string error;
    try { compare(*gothic.world()); }
    catch(const std::exception& failure) { error=failure.what(); }
    const auto expected=mutation==4 || mutation==5 ? "game random generator differs" : "live NPC differs";
    if(error.find(expected)==std::string::npos)
      throw std::runtime_error("live NPC mutation "+std::to_string(mutation+1)+" was not rejected by parity: "+error);
    write(file,original);
    }
  std::puts("Live NPC mutations: attribute, call order, missing call, untouched id, missing/extra random call, helper draw before the previous chain step rejected");
  }
void helperMutations(const fs::path& file,Gothic& gothic,void (*compare)(World&)) {
  std::ifstream input(file,std::ios::binary);
  const std::string original{std::istreambuf_iterator<char>(input),{}};
  struct Restore {
    fs::path file;
    const std::string& bytes;
    ~Restore() { write(file,bytes); }
    } restore{file,original};
  for(unsigned mutation=0;mutation<2;++mutation) {
    rapidjson::Document document;
    document.Parse(original.data(),original.size());
    if(document.HasParseError()) throw std::runtime_error("invalid synthetic helper definitions JSON");
    bool changed=false;
    for(auto& record:document["records"].GetArray()) {
      const auto name=std::string_view(record["symbol"].GetString());
      if(mutation==0 && name=="B_SETFIGHTSKILLS") {
        record["parameters"]["talents"][0]["thresholds"][1]["skill"].SetInt(2);
        changed=true;
        }
      if(mutation==1 && name=="B_CREATEAMBIENTINV") {
        for(auto& guild:record["parameters"]["guilds"].GetArray())
          for(auto& roll:guild["table"]["rolls"].GetArray()) roll[0].Swap(roll[1]);
        for(auto& roll:record["parameters"]["fallback"]["rolls"].GetArray()) roll[0].Swap(roll[1]);
        changed=true;
        }
      }
    if(!changed) throw std::runtime_error("synthetic helper mutation target missing");
    rapidjson::StringBuffer bytes;
    rapidjson::Writer<rapidjson::StringBuffer> writer(bytes);
    if(!document.Accept(writer)) throw std::runtime_error("cannot encode synthetic helper mutation");
    write(file,{bytes.GetString(),bytes.GetSize()});
    gothic.clearGame().reset();
    gothic.setGame(std::make_unique<GameSession>("NPC_CONTRACT.ZEN"));
    std::string error;
    try { compare(*gothic.world()); }
    catch(const std::exception& failure) { error=failure.what(); }
    if(error.find("live NPC differs")==std::string::npos)
      throw std::runtime_error("live helper mutation "+std::to_string(mutation+1)+" was not rejected by parity: "+error);
    write(file,original);
    }
  std::puts("Live helper mutations: wrong skill tier and reversed inventory calls rejected without game data");
  }

}

void npcPetzCrimeParity(World& world,Npc& witness,Npc& hero,bool& enabled) {
  using Contract=NpcDefinitionContract;
  auto& script=world.script();
  auto& vm=script.getVm();
  auto& record=witness.handle();
  const auto witnessHandle=std::static_pointer_cast<daedalus::INpc>(witness.handlePtr());
  const auto heroHandle=std::static_pointer_cast<daedalus::INpc>(hero.handlePtr());
  const auto symbol=[&](std::string_view name) -> daedalus::DaedalusSymbol& {
    auto* result=vm.find_symbol_by_name(name);
    if(result==nullptr) throw std::runtime_error("Missing crime fixture symbol: "+std::string(name));
    return *result;
    };
  const auto date=int32_t(world.time().day());
  const auto murder=symbol("CRIME_MURDER").get_int(),theft=symbol("CRIME_THEFT").get_int();
  const auto attack=symbol("CRIME_ATTACK").get_int(),sheep=symbol("CRIME_SHEEPKILLER").get_int();
  const auto none=symbol("CRIME_NONE").get_int();
  const auto addPc=symbol("B_ADDPETZCRIME").address(),removePc=symbol("B_DELETEPETZCRIME").address();
  auto& input=symbol("B_ADDPETZCRIME.CRIME");
  const auto savedInput=input.get_int();
  const auto savedGuild=record.guild;
  const auto savedType=record.type;
  const auto savedAivar=std::to_array(record.aivar);
  const bool wasEnabled=enabled;
  struct Global {
    daedalus::DaedalusSymbol& symbol;
    int32_t& owned;
    int32_t saved;
    };
  std::vector<Global> globals;
  auto& levels=Contract::crimeAbsolutionLevels(script);
  auto& counters=Contract::crimeCounters(script);
  size_t levelIndex=0,counterIndex=0;
  for(std::string_view area : {"OLDCAMP","CITY","MONASTERY","FARM","BL"}) {
    auto& level=symbol("ABSOLUTIONLEVEL_"+std::string(area));
    globals.push_back({level,levels.at(levelIndex++),level.get_int()});
    for(std::string_view crime : {"MURDER","THEFT","ATTACK","SHEEPKILLER"}) {
      if(area=="BL" && crime=="SHEEPKILLER") continue;
      auto& counter=symbol("PETZCOUNTER_"+std::string(area)+"_"+std::string(crime));
      globals.push_back({counter,counters.at(counterIndex++),counter.get_int()});
      }
    }
  const auto savedValue=[](size_t index) { return int32_t(index)-(index<12 ? 12 : 11); };
  auto& location=symbol("B_GRANTABSOLUTION.LOCATION");
  const auto savedLocation=location.get_int();
  for(const bool legacy : {true,false}) {
    std::vector<uint8_t> bytes;
    Tempest::MemWriter writer(bytes);
    {
      Serialize out(writer);
      out.setEntry("game/daedalus");
      out.write(uint32_t(globals.size()));
      for(size_t i=0; i<globals.size(); ++i) {
        auto& [global,owned,saved]=globals[i];
        owned=savedValue(i);
        if(global.get_int()!=owned)
          throw std::runtime_error("Crime memory: Daedalus must read the engine-owned value of "+global.name());
        if(legacy)
          out.write(uint32_t(daedalus::DaedalusDataType::INT),global.name(),uint32_t(1),savedValue(i));
        else
          Contract::saveCrimeGlobal(script,out,global);
        }
    }
    vm.call_function<void>("B_GRANTABSOLUTION",symbol("LOC_ALL").get_int());
    vm.call_function<void>("B_GRANTABSOLUTION",symbol("LOC_BL").get_int());
    location.set_int(savedLocation);
    for(size_t i=0; i<globals.size(); ++i) {
      const auto& [global,owned,saved]=globals[i];
      const int32_t expected=global.name().starts_with("ABSOLUTIONLEVEL_") ? savedValue(i)+1 : 0;
      if(owned!=expected || global.get_int()!=expected)
        throw std::runtime_error("Crime memory: stock absolution must update "+global.name());
      }
    Tempest::MemReader reader(bytes);
    Serialize in(reader);
    if(!in.setEntry("game/daedalus")) throw std::runtime_error("Crime memory: save must use the original entry");
    script.loadVar(in);
    for(size_t i=0; i<globals.size(); ++i) {
      const auto& [global,owned,saved]=globals[i];
      if(owned!=savedValue(i) || global.get_int()!=owned)
        throw std::runtime_error("Crime memory: save must restore "+global.name());
      }
    }
  std::vector<std::pair<daedalus::DaedalusSymbol*,std::shared_ptr<daedalus::DaedalusInstance>>> references;
  for(const char* name : {"B_ADDPETZCRIME.SLF","B_DELETEPETZCRIME.SLF","B_GETPLAYERCRIME.SLF",
                          "B_GETCURRENTABSOLUTIONLEVEL.SLF","B_MEMORIZEPLAYERCRIME.SLF","B_MEMORIZEPLAYERCRIME.TAETER",
                          "C_NPCBELONGSTOOLDCAMP.SLF","C_NPCBELONGSTOCITY.SLF","C_NPCBELONGSTOMONASTERY.SLF",
                          "C_NPCBELONGSTOFARM.SLF","C_NPCBELONGSTOBL.SLF"}) {
    auto& ref=symbol(name);
    references.emplace_back(&ref,ref.get_instance());
    }
  auto& reported=symbol("B_MEMORIZEPLAYERCRIME.CRIME");
  const auto savedReported=reported.get_int();
  struct Case {
    Guild guild;
    NpcType type;
    int32_t previous,age,level,crime;
    const char* added;
    const char* removed;
    const char* additionallyAdded=nullptr;
    const char* additionallyRemoved=nullptr;
    };
  const Case cases[] = {
    {GIL_VLK,NpcType::G2_AMBIENT,attack,2,3,theft,"PETZCOUNTER_CITY_THEFT","PETZCOUNTER_CITY_ATTACK"},
    {GIL_VLK,NpcType::G2_BL_AMBIENT,theft,7,3,murder,"PETZCOUNTER_CITY_MURDER","PETZCOUNTER_CITY_THEFT","PETZCOUNTER_BL_MURDER","PETZCOUNTER_BL_THEFT"},
    {GIL_MIL,NpcType::G2_OC_MAIN,attack,3,3,murder,"PETZCOUNTER_OLDCAMP_MURDER",nullptr},
    {GIL_NOV,NpcType::G2_AMBIENT,murder,0,2,theft,"PETZCOUNTER_MONASTERY_THEFT",nullptr},
    {GIL_BAU,NpcType::G2_AMBIENT,theft,7,3,murder,"PETZCOUNTER_FARM_MURDER","PETZCOUNTER_FARM_THEFT"},
    {GIL_BDT,NpcType::G2_BL_MAIN,none,0,7,sheep,nullptr,nullptr},
    {GIL_NONE,NpcType::G2_AMBIENT,none,0,0,murder,nullptr,nullptr},
    {GIL_VLK,NpcType::G2_AMBIENT,attack,2,3,attack,nullptr,nullptr},
    };
  for(const auto& c : cases) {
    std::vector<std::shared_ptr<daedalus::DaedalusInstance>> original;
    for(const bool native : {false,true}) {
      enabled=native;
      record.guild=c.guild;
      record.type=c.type;
      record.aivar[1]=c.previous;
      record.aivar[2]=date-c.age;
      record.aivar[46]=c.level;
      input.set_int(-1);
      for(const auto& [global,owned,saved] : globals) {
        const std::string_view name=global.name();
        global.set_int(name=="ABSOLUTIONLEVEL_BL" ? 7 : (name.starts_with("ABSOLUTIONLEVEL_") ? 3 : 0));
        }
      for(const auto& [ref,saved] : references) ref->set_instance(heroHandle);
      size_t bytecode=0;
      vm.observe_instructions([&](const auto&) { bytecode+=vm.pc()==addPc || vm.pc()==removePc; });
      vm.call_function<void>("B_MEMORIZEPLAYERCRIME",witnessHandle,heroHandle,c.crime);
      vm.observe_instructions({});
      const bool equal=c.previous==c.crime;
      const int32_t level=c.guild==GIL_NONE ? 0 : (c.guild==GIL_BDT ? 7 : 3);
      if(bytecode!=(native || equal ? 0u : 2u) || record.aivar[1]!=c.crime || record.aivar[2]!=date ||
         record.aivar[46]!=level || input.get_int()!=(equal ? -1 : c.crime))
        throw std::runtime_error("Crime memory: callbacks, same-crime refresh and recorded witness details match stock");
      for(const auto& [global,owned,saved] : globals) {
        const std::string_view name=global.name();
        if(!name.starts_with("PETZCOUNTER_")) continue;
        const auto matches=[&](const char* expected) { return expected!=nullptr && name==expected; };
        const int32_t expected=matches(c.added) || matches(c.additionallyAdded) ? 1 :
                               (matches(c.removed) || matches(c.additionallyRemoved) ? -1 : 0);
        if(global.get_int()!=expected || owned!=expected)
          throw std::runtime_error("Crime memory: unexpected counter "+std::string(name));
        }
      for(size_t i=0; i<references.size(); ++i) {
        const auto value=references[i].first->get_instance();
        if(!native) original.push_back(value);
        else if(original[i]!=value) throw std::runtime_error("Crime memory: saved witness references differ from stock");
        }
      }
    }
  enabled=wasEnabled;
  record.guild=savedGuild;
  record.type=savedType;
  std::copy(savedAivar.begin(),savedAivar.end(),record.aivar);
  input.set_int(savedInput);
  reported.set_int(savedReported);
  for(const auto& [global,owned,saved] : globals) owned=saved;
  for(const auto& [ref,saved] : references) ref->set_instance(saved);
  std::puts("Crime memory: engine-owned counters, absolution, legacy saves, expiry and witness references match the VM");
  }

// The same live comparator as the installed-game corpus runs on a complete, tiny own-format
// base. The fixture needs neither Gothic archives nor a graphics or sound device.
void npcLiveSynthetic(void (*compare)(World&), void (*disabled)(World&),
                      void (*placement)(World&), void (*placementDisabled)(World&), void (*activities)(World&)) {
  const auto root=fs::temp_directory_path()/("gothic-npc-live-"+
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  struct Cleanup {
    fs::path path;
    ~Cleanup() { std::error_code ignored; fs::remove_all(path,ignored); }
    } cleanup{root};
  const auto game=root/"game",base=root/"base";
  fs::create_directories(game/"Data");
  write(game/"system/Gothic.ini","[GAME]\nlanguage=-1\n");
  write(game/"system/SystemPack.ini","");
  emptyIndices(base);
  world(base);
  const auto definitions=program(base);
  BaseImport::writeBaseMetadata(base,BaseImport::identify(game,BaseImport::Files{}));
  Environment environment;
  environment.set("OPENGOTHIC_BASE",base.string());
  environment.set("OPENGOTHIC_BASE_FINGERPRINT","");
  environment.set("OPENGOTHIC_BASE_CACHE",(root/"cache").string());
  environment.set("OPENGOTHIC_BASE_STORE",(root/"store").string());
  environment.set("OPENGOTHIC_VIDEOS_FINGERPRINT","");
  environment.set("OPENGOTHIC_CONTENT","");
  environment.set("OPENGOTHIC_TEXTURES","");
  environment.set("OPENGOTHIC_NPC_RECORDS",std::nullopt);
  environment.set("OPENGOTHIC_NPC_HELPER_RECORDS",std::nullopt);
  environment.set("OPENGOTHIC_ITEM_RECORDS",std::nullopt);
  environment.set("OPENGOTHIC_ROUTINE_RECORDS",std::nullopt);
  environment.set("OPENGOTHIC_PLACEMENT_RECORDS",std::nullopt);
  environment.set("OPENGOTHIC_ACTIVITY_RECORDS",std::nullopt);
  const auto directory=game.string();
  const char* args[]={"gothic-placed-items-contract","-g",directory.c_str(),"-benchmark","ci"};
  CommandLine commandLine(5,args);
  Resources resources;
  Gothic gothic;
  gothic.isNpcInDialogFn=[](const Npc*) { return false; };
  gothic.setupGlobalScripts();
  GameMusic music;
  gothic.setGame(std::make_unique<GameSession>("NPC_CONTRACT.ZEN"));
  compare(*gothic.world());
  npcDayRandom(*gothic.world());
  placement(*gothic.world());
  activities(*gothic.world());
  mutations(definitions,gothic,compare);
  helperMutations(definitions.parent_path()/"helpers.json",gothic,compare);
  gothic.clearGame().reset();
  environment.set("OPENGOTHIC_NPC_RECORDS","0");
  environment.set("OPENGOTHIC_NPC_HELPER_RECORDS","0");
  environment.set("OPENGOTHIC_ITEM_RECORDS","0");
  environment.set("OPENGOTHIC_ROUTINE_RECORDS","0");
  environment.set("OPENGOTHIC_PLACEMENT_RECORDS","0");
  environment.set("OPENGOTHIC_ACTIVITY_RECORDS","0");
  gothic.setGame(std::make_unique<GameSession>("NPC_CONTRACT.ZEN"));
  disabled(*gothic.world());
  placementDisabled(*gothic.world());
  gothic.clearGame().reset();
  std::puts("Synthetic live NPC definitions: normal GameSession and engine externals, no game data");
  }

namespace NpcWorldFixture {

static void oldWorldEntry(World& world,Npc& living) {
  if(NpcSnapshot::load(baseProgramIndex().dir.parent_path()).sha256!=
     "a1918ecb7239b6f48cb98dda5e6ff7eb7ab8099085b3d7adec603c1431de641c")
    return;

  auto& script=world.script();
  auto& vm=script.getVm();
  auto* root=script.findSymbol("B_ENTER_OLDWORLD");
  auto* biff=script.findSymbol("DJG_713_BIFF");
  auto* bilgot=script.findSymbol("VLK_4120_BILGOT");
  auto* biffAlias=script.findSymbol("BIFF");
  auto* bilgotAlias=script.findSymbol("BILGOT");
  const auto biffBefore=biff->get_instance(), bilgotBefore=bilgot->get_instance();
  const auto biffAliasBefore=biffAlias->get_instance(), bilgotAliasBefore=bilgotAlias->get_instance();
  auto& refusal=NpcDefinitionContract::refusalDeadline(living);
  const auto refusalBefore=refusal;
  std::map<std::string_view,int32_t> before;
  for(const auto* name:{"KAPITEL","CURRENTLEVEL","DJG_BIFFPARTY","DJG_BIFF_HALBEHALBE","MIS_RESCUEBILGOT",
                        "ENTEROW_KAPITEL1","ENTEROW_KAPITEL2"})
    before.emplace(name,script.findSymbol(name)->get_int());
  const auto* configured=std::getenv("OPENGOTHIC_PLACEMENT_RECORDS");
  const auto oldSetting=configured ? std::optional<std::string>(configured) : std::nullopt;
  Npc dead(world,bilgot->index(),"OW_DJG_ROCKCAMP_01");
  require(dead.startState(script.findFunction("ZS_DEAD"),""),"Old World entry fixture must set up a dead NPC");
  const auto livingHandle=std::static_pointer_cast<daedalus::INpc>(living.handlePtr());
  const auto deadHandle=std::static_pointer_cast<daedalus::INpc>(dead.handlePtr());
  auto* chapter=script.findSymbol("KAPITEL");
  auto* level=script.findSymbol("CURRENTLEVEL");
  auto* rescue=script.findSymbol("MIS_RESCUEBILGOT");
  const auto oldLevel=script.intSymbol("OLDWORLD_ZEN",0), newLevel=script.intSymbol("NEWWORLD_ZEN",0);

  const auto run=[&](bool native,int refusalSeconds,bool bilgotDead) {
    setenv("OPENGOTHIC_PLACEMENT_RECORDS",native ? "1" : "0",1);
    biff->set_instance(livingHandle);
    bilgot->set_instance(bilgotDead ? deadHandle : livingHandle);
    refusal=0;
    chapter->set_int(1);
    level->set_int(newLevel);
    script.findSymbol("ENTEROW_KAPITEL1")->set_int(1);
    script.findSymbol("ENTEROW_KAPITEL2")->set_int(1);
    script.findSymbol("DJG_BIFFPARTY")->set_int(refusalSeconds!=0);
    script.findSymbol("DJG_BIFF_HALBEHALBE")->set_int(refusalSeconds==500);
    rescue->set_int(refusalSeconds==0 ? 0 : script.intSymbol("LOG_RUNNING",0));

    std::vector<std::pair<std::string,int32_t>> calls;
    const auto observer=vm.add_call_observer([&](const auto& symbol) {
      if(symbol.name()=="B_INITNPCGLOBALS" || symbol.name().starts_with("B_ENTER_OLDWORLD_KAPITEL_"))
        calls.emplace_back(symbol.name(),level->get_int());
      if(symbol.name()=="B_ENTER_OLDWORLD_KAPITEL_1")
        chapter->set_int(2);
      });
    vm.observe_externals([&](const auto& symbol) {
      if(symbol.name()!="NPC_ISDEAD" && symbol.name()!="NPC_SETREFUSETALK")
        return;
      const uint16_t depth=symbol.name()=="NPC_ISDEAD" ? 0 : 1;
      auto* alias=std::get<daedalus::DaedalusSymbol*>(vm.stack_frame(depth).value);
      calls.emplace_back(symbol.name()+":"+alias->name(),level->get_int());
      });
    size_t entries=0;
    vm.observe_instructions([&](const auto&) { if(vm.pc()==root->address()) ++entries; });
    const auto pc=vm.pc();
    const auto context=vm.unsafe_get_gi();
    vm.push_int(0x12345678);
    vm.unsafe_call_bl(root);
    const auto sentinel=vm.pop_int();
    vm.observe_instructions({});
    vm.observe_externals({});
    vm.remove_call_observer(observer);

    require(entries==(native ? 0u : 1u) && sentinel==0x12345678 && vm.pc()==pc && vm.unsafe_get_gi()==context,
            "Old World entry must use the selected executor and preserve VM context");
    const auto quest=refusalSeconds==0 ? 0 : script.intSymbol(bilgotDead ? "LOG_FAILED" : "LOG_RUNNING",0);
    require(chapter->get_int()==2 && level->get_int()==oldLevel &&
            rescue->get_int()==quest && refusal==(refusalSeconds==0 ? 0 : world.tickCount()+uint64_t(refusalSeconds)*1000),
            "Old World entry must apply the live chapter, Biff refusal time and Bilgot rescue result");
    return calls;
    };
  for(const auto [seconds,deadBilgot]:{std::pair{500,true},std::pair{300,false},std::pair{0,true}}) {
    std::vector<std::pair<std::string,int32_t>> expected={
      {"B_INITNPCGLOBALS",newLevel},{"B_ENTER_OLDWORLD_KAPITEL_1",newLevel},
      {"B_ENTER_OLDWORLD_KAPITEL_2",newLevel},{"B_INITNPCGLOBALS",oldLevel},{"NPC_ISDEAD:BIFF",oldLevel}};
    if(seconds!=0) expected.emplace_back("NPC_SETREFUSETALK:BIFF",oldLevel);
    expected.emplace_back("NPC_ISDEAD:BILGOT",oldLevel);
    require(run(false,seconds,deadBilgot)==expected && run(true,seconds,deadBilgot)==expected,
            "Old World entry: stock and native must preserve both alias passes, chapter order and eager death checks");
    }

  refusal=refusalBefore;
  biff->set_instance(biffBefore);
  bilgot->set_instance(bilgotBefore);
  biffAlias->set_instance(biffAliasBefore);
  bilgotAlias->set_instance(bilgotAliasBefore);
  for(const auto& [name,value]:before)
    script.findSymbol(name)->set_int(value);
  if(oldSetting) setenv("OPENGOTHIC_PLACEMENT_RECORDS",oldSetting->c_str(),1);
  else unsetenv("OPENGOTHIC_PLACEMENT_RECORDS");
  std::puts("Old World entry: live chapter gates, eager death checks, Biff refusal and Bilgot rescue match stock");
  }

static void diegoRouteBoundaries(World& world,Npc& context) {
  using Contract=NpcDefinitionContract;
  auto& script=world.script();
  auto& vm=script.getVm();
  auto& enabled=Contract::activityEventsOn(script);
  const auto savedEnabled=enabled;
  auto* hero=world.player();
  const auto* angar=world.findWayPoint("SPAWN_OW_SHADOWBEAST_10_01");
  const auto* ice=world.findWayPoint("OW_ICEREGION_ENTRANCE_01");
  require(hero && angar && ice,"Diego's route needs the original OldWorld waypoints");
  const auto savedPosition=hero->position();
  auto* check=vm.find_symbol_by_name("C_DIEGOTOOFAR");
  auto* tolerance=vm.find_symbol_by_name("C_DIEGOTOOFAR.TOLERANCE");
  require(check && tolerance,"the route fixture uses the original function and argument slot");
  const auto savedTolerance=tolerance->get_int();
  const auto savedSelf=vm.global_self()->get_instance();
  vm.global_self()->set_instance(std::static_pointer_cast<daedalus::INpc>(context.handlePtr()));
  struct Site { const WayPoint* point; int32_t offset, tolerance, region; };
  for(const Site site : {Site{angar,3999,0,1}, Site{angar,4000,0,0}, Site{angar,4000,1000,1}, Site{ice,0,0,2}}) {
    hero->setPosition(site.point->position()+Tempest::Vec3(float(site.offset),0,0));
    int32_t original=0;
    for(bool native : {false,true}) {
      enabled=native;
      tolerance->set_int(-1);
      size_t instructions=0;
      vm.observe_instructions([&](const auto&) { instructions+=vm.pc()==check->address(); });
      const auto region=vm.call_function<int32_t>(check,site.tolerance);
      vm.observe_instructions({});
      require(instructions==size_t(!native) && tolerance->get_int()==site.tolerance,
              "Diego's route query preserves its argument and uses the selected executor");
      if(site.region!=0)
        require(region==site.region,"Diego's route preserves the original nonzero region code");
      else
        require(region!=1,"Diego's route excludes the strict Angar radius boundary");
      if(native) require(region==original,"Diego's route agrees with the original at each position and tolerance");
      else original=region;
      }
    }
  auto* diego=activityNpc(world,"DIEGOOW");
  const bool added=diego==nullptr;
  if(added)
    diego=world.addNpc(script.findSymbolIndex("DIEGOOW"),angar->position());
  require(diego && !diego->isDead(),"Diego's follower fixture needs the original actor");
  const auto savedDiego=savedNpc(*diego);
  const auto follow=script.findFunction("B_ASSESSFOLLOWPLAYER");
  const auto followPc=vm.find_symbol_by_name("B_ASSESSFOLLOWPLAYER")->address();
  for(bool native : {false,true}) {
    enabled=native;
    loadSavedNpc(*diego,savedDiego);
    hero->setPosition(angar->position());
    diego->setPosition(angar->position()+Tempest::Vec3(600,0,0));
    diego->setProcessPolicy(NpcProcessPolicy::AiNormal);
    diego->setExternalControl(false);
    diego->clearAiQueue();
    diego->aiPush(AiQueue::aiWait(713));
    auto& state=Contract::aiState(*diego);
    state.funcIni=script.findFunction("ZS_FOLLOW_PLAYER");
    state.started=true;
    diego->handle().aivar[19]=0;
    diego->handle().aivar[80]=500;
    diego->setPerceptionEnable(PERC_ASSESSPLAYER,follow.ptr);
    size_t followerBytecode=0,routeBytecode=0;
    vm.observe_instructions([&](const auto&) {
      followerBytecode+=vm.pc()==followPc;
      routeBytecode+=vm.pc()==check->address();
      });
    const bool delivered=diego->passPerception(*hero,nullptr,PERC_ASSESSPLAYER);
    vm.observe_instructions({});
    auto queue=Contract::queue(*diego);
    require(delivered && followerBytecode==size_t(!native) && routeBytecode==size_t(!native) &&
            diego->handle().aivar[19]==1 && queue.size()==2 && queue.pop().act==AI_StandUp,
            "Diego must cancel the approach when the hero reaches the next route boundary");
    const auto waitLine=queue.pop();
    require(waitLine.act==AI_OutputSvm && waitLine.s0=="$SC_HEYWAITASECOND" && waitLine.target==hero,
            "Diego must ask the hero to wait after standing up");
    }
  if(added)
    world.removeNpc(*diego);
  else
    loadSavedNpc(*diego,savedDiego);
  hero->setPosition(savedPosition);
  vm.global_self()->set_instance(savedSelf);
  tolerance->set_int(savedTolerance);
  enabled=savedEnabled;
  std::puts("Diego route: original and native region codes, dialogue tolerance and follower warning agree");
  }

void companionPlayerPerceptions(World& world,Npc& npc,Npc& hero) {
  using Contract=NpcDefinitionContract;
  auto& script=world.script();
  auto& vm=script.getVm();
  auto& enabled=Contract::activityEventsOn(script);
  const auto savedEnabled=enabled;
  const auto savedNpcState=savedNpc(npc);
  const auto savedHeroState=savedNpc(hero);
  const auto* point=world.findWayPoint(npc.handle().wp);
  require(point!=nullptr,"companion assessment needs the original NPC waypoint");
  const auto follow=script.findFunction("B_ASSESSFOLLOWPLAYER");
  const auto guide=script.findFunction("B_ASSESSGUIDEPLAYER");
  const auto guideLoop=script.findFunction("ZS_GUIDE_PLAYER_LOOP");
  const auto followPc=vm.find_symbol_by_name("B_ASSESSFOLLOWPLAYER")->address();
  const auto guidePc=vm.find_symbol_by_name("B_ASSESSGUIDEPLAYER")->address();
  const auto playerPc=vm.find_symbol_by_name("B_ASSESSPLAYER")->address();
  enum class Phase { Follow, Guide, GuideLoop };
  struct Site { Phase phase; int32_t distance; bool handoff, hostile; };
  for(const auto site : {Site{Phase::Follow,499,false,false},Site{Phase::Follow,500,true,false},
                         Site{Phase::Guide,800,true,false},Site{Phase::Guide,1200,false,true},
                         Site{Phase::GuideLoop,1201,false,true}}) {
    std::vector<std::string> original;
    for(bool native : {false,true}) {
      enabled=native;
      loadSavedNpc(npc,savedNpcState);
      loadSavedNpc(hero,savedHeroState);
      npc.setExternalControl(false);
      npc.setProcessPolicy(NpcProcessPolicy::AiNormal);
      npc.setPosition(point->position());
      hero.setPosition(point->position()+Tempest::Vec3(float(site.distance),0,0));
      npc.setAttitude(site.hostile ? ATT_HOSTILE : ATT_FRIENDLY);
      npc.handle().aivar[19]=0;
      npc.handle().aivar[61]=site.hostile ? 1 : 0;
      npc.handle().aivar[80]=500;
      npc.clearAiQueue();
      npc.aiPush(AiQueue::aiWait(713));
      const bool following=site.phase==Phase::Follow;
      const auto callback=following ? follow : guide;
      npc.setPerceptionEnable(PERC_ASSESSPLAYER,callback.ptr);
      size_t wrapperBytecode=0,playerBytecode=0;
      vm.observe_instructions([&](const auto&) {
        wrapperBytecode+=vm.pc()==(following ? followPc : guidePc);
        playerBytecode+=vm.pc()==playerPc;
        });
      const auto delivered=site.phase==Phase::GuideLoop
        ? script.invokeState(&npc,&hero,nullptr,guideLoop)==LOOP_CONTINUE
        : npc.passPerception(hero,nullptr,PERC_ASSESSPLAYER);
      vm.observe_instructions({});
      require(delivered && wrapperBytecode==size_t(!native) && playerBytecode==size_t(!native && site.handoff),
              "companion assessment must select the same specialized or normal branch");
      if(!site.handoff) {
        auto queue=Contract::queue(npc);
        require(queue.size()==(following ? 1u : 2u) && queue.pop().act==AI_StandUp,
                "approaching a follower or falling behind a guide replaces its previous queue");
        if(!following) {
          const auto turn=queue.pop();
          require(turn.act==AI_TurnToNpc && turn.target==&hero,"the guide turns to the hero");
          }
        require(npc.handle().aivar[19]==int32_t(following) &&
                npc.handle().aivar[61]==int32_t(site.hostile && site.distance<=1200),
                "the follower reaches its position and the guide keeps the strict hostile cutoff");
        }
      auto queue=activityQueue(npc);
      if(native) require(queue==original,"companion assessment preserves the original action order");
      else original=std::move(queue);
      }
    }
  loadSavedNpc(npc,savedNpcState);
  loadSavedNpc(hero,savedHeroState);
  enabled=savedEnabled;
  std::puts("Companion assessments: follower arrival, ordinary player handoff and guide wait agree with the VM");
  }

void fightSoundPerceptions(World& world,Npc& npc,Npc& hero) {
  using Contract=NpcDefinitionContract;
  auto& script=world.script();
  auto& vm=script.getVm();
  auto& enabled=Contract::activityEventsOn(script);
  const auto savedEnabled=enabled;
  auto* victim=activityNpc(world,"BAU_934_BABERA");
  auto* party=activityNpc(world,"BAU_935_BRONKO");
  require(victim && victim!=&npc && !victim->isDead() && party && party!=&npc && !party->isDead(),
          "fight sound: the living witnesses and participants exist");
  const auto savedListener=savedNpc(npc);
  const auto savedVictim=savedNpc(*victim);
  const auto savedHero=savedNpc(hero);
  const auto savedParty=savedNpc(*party);
  const auto restore=[&] {
    loadSavedNpc(npc,savedListener);
    loadSavedNpc(*victim,savedVictim);
    loadSavedNpc(hero,savedHero);
    loadSavedNpc(*party,savedParty);
    };
  const auto here=npc.position();
  const auto nearby=hero.position();
  const auto attack=script.findFunction("ZS_ATTACK");
  const auto watch=script.findFunction("ZS_WATCHFIGHT");
  const auto sound=script.findFunction("B_ASSESSFIGHTSOUND");
  const auto guideSound=script.findFunction("B_ASSESSGUIDEFIGHTSOUND");
  const auto followSound=script.findFunction("B_ASSESSFOLLOWFIGHTSOUND");
  const auto soundPc=vm.find_symbol_by_name("B_ASSESSFIGHTSOUND")->address();
  const auto guidePc=vm.find_symbol_by_name("B_ASSESSGUIDEFIGHTSOUND")->address();
  const auto followPc=vm.find_symbol_by_name("B_ASSESSFOLLOWFIGHTSOUND")->address();
  enum class Case { NoVictim, Distant, Nailed, Monster, Room, Watch, Guide, FollowerParty, FollowerOther, GuideParty };
  for(auto kind : {Case::NoVictim,Case::Distant,Case::Nailed,Case::Monster,Case::Room,Case::Watch,
                   Case::Guide,Case::FollowerParty,Case::FollowerOther,Case::GuideParty}) {
    std::vector<std::string> original;
    for(bool native : {false,true}) {
      enabled=native;
      restore();
      npc.setProcessPolicy(NpcProcessPolicy::AiNormal);
      npc.setExternalControl(false);
      npc.clearAiQueue();
      npc.aiPush(AiQueue::aiWait(713));
      npc.setAttitude(ATT_FRIENDLY);
      Contract::aiState(npc).funcIni=script.findFunction("ZS_STAND_WP");
      npc.handle().guild=GIL_BAU;
      npc.handle().aivar[61]=0;
      hero.handle().guild=GIL_NONE;
      hero.handle().fight_tactic=0;
      hero.handle().aivar[9]=kind==Case::Room ? script.intSymbol("AR_CLEARROOM",0) : 0;
      victim->handle().guild=kind==Case::Monster ? int32_t(GIL_WOLF) : int32_t(GIL_BAU);
      victim->handle().fight_tactic=kind==Case::Nailed ? script.intSymbol("FAI_NAILED",1) : 0;
      victim->handle().aivar[9]=0;
      victim->handle().aivar[15]=0;
      const bool following=kind==Case::FollowerParty || kind==Case::FollowerOther;
      const bool guiding=kind==Case::Guide || kind==Case::GuideParty;
      auto& source=following || kind==Case::GuideParty ? *party : hero;
      party->handle().guild=GIL_BAU;
      party->handle().aivar[9]=0;
      party->handle().aivar[15]=kind==Case::FollowerOther ? 0 : 1;
      party->handle().fight_tactic=0;
      party->setPosition(nearby);
      victim->setPosition(kind==Case::Distant ? here+Tempest::Vec3(2000,0,0) : nearby);
      hero.setPosition(kind==Case::Distant ? here+Tempest::Vec3(2200,0,0) : nearby);
      npc.setPerceptionEnable(PERC_ASSESSFIGHTSOUND,(following ? followSound : guiding ? guideSound : sound).ptr);

      size_t rootBytecode=0,wrapperBytecode=0;
      vm.observe_instructions([&](const auto&) {
        rootBytecode+=vm.pc()==soundPc;
        wrapperBytecode+=vm.pc()==guidePc || vm.pc()==followPc;
        });
      const auto delivered=npc.passPerception(source,kind==Case::NoVictim ? nullptr : victim,PERC_ASSESSFIGHTSOUND);
      vm.observe_instructions({});
      const bool handedOff=kind!=Case::GuideParty && kind!=Case::FollowerOther;
      require(delivered && rootBytecode==size_t(!native && handedOff) &&
              wrapperBytecode==size_t(!native && (following || guiding)),
              "fight sound: the specialized callbacks preserve the stock party and player filters");
      const auto* next=npc.aiQueueFront();
      const bool startsAttack=kind==Case::Monster || kind==Case::Room;
      const bool startsWatch=kind==Case::Watch || kind==Case::Guide || kind==Case::FollowerParty;
      require(next && (startsAttack || startsWatch ? next->act==AI_StartState && next->func==(startsAttack ? attack : watch) &&
                       next->target==&source && next->victim==victim && next->i0==0 && !npc.hasPerc(PERC_ASSESSFIGHTSOUND)
                     : next->act==AI_Wait && npc.hasPerc(PERC_ASSESSFIGHTSOUND)),
              "fight sound: the listener preserves the original queue outcome, source and victim");
      if(startsAttack) {
        const auto reason=kind==Case::Monster ? "AR_MONSTERVSHUMAN" : "AR_GUARDCALLEDTOROOM";
        require(npc.target()==victim && npc.handle().aivar[9]==script.intSymbol(reason,-1),
                "fight sound: the attack chooses the victim with the correct reason");
        }
      auto queue=activityQueue(npc);
      if(native) require(original==queue,"fight sound: stock and native queues agree");
      else original=std::move(queue);
      }
    }
  restore();
  enabled=savedEnabled;
  std::puts("Fight sound: direct, guide and follower perception preserve source, victim, party filters and action");
  }

void oldWorldNpcCases(World& world,Npc& npc) {
  oldWorldEntry(world,npc);
  diegoRouteBoundaries(world,npc);
  }

void dragonIslandChapterWorld(World& world) {
  std::puts("Dragon Island placement: four captain and crew combinations match the original calls and world state");
  auto& script=world.script();
  auto& vm=script.getVm();
  auto* root=script.findSymbol("B_KAPITELWECHSEL");
  auto* savedChapter=script.findSymbol("B_KAPITELWECHSEL.NEUES_KAPITEL");
  auto* savedLevel=script.findSymbol("B_KAPITELWECHSEL.AKTUELLES_LEVEL_ZEN");
  auto* chapter=script.findSymbol("KAPITEL");
  auto* ambient=script.findSymbol("XP_AMBIENT");
  auto* alchemy=script.findSymbol("PLAYER_TALENT_ALCHEMY");
  auto* hero=world.player();
  auto* pedro=activityNpc(world,"NOV_600_PEDRO_DI");
  require(root && savedChapter && savedLevel && chapter && ambient && alchemy && hero && pedro,
          "chapter switch fixture needs the stock actors and symbols");
  const auto heroHandle=std::static_pointer_cast<daedalus::INpc>(hero->handlePtr());
  const auto pedroHandle=std::static_pointer_cast<daedalus::INpc>(pedro->handlePtr());
  const auto oldWorld=script.intSymbol("OLDWORLD_ZEN",0), newWorld=script.intSymbol("NEWWORLD_ZEN",0);
  const auto island=script.intSymbol("DRAGONISLAND_ZEN",0);
  // The world is disposed after this fixture. Observe the UI arguments without opening a chapter screen.
  vm.register_external("INTRODUCECHAPTER",[](std::string_view,std::string_view,std::string_view,std::string_view,int32_t) {});
  vm.override_function("B_ENTER_OLDWORLD",[&vm,root,newWorld] {
    vm.push_int(5);
    vm.push_int(newWorld);
    vm.unsafe_call_bl(root);
    });
  vm.override_function("B_ENTER_NEWWORLD",[] {});

  const auto journal=[&] {
    std::vector<uint8_t> bytes;
    Tempest::MemWriter writer(bytes);
    {
      Serialize out(writer);
      out.setEntry("game/quests");
      script.saveQuests(out);
    }
    return bytes;
    };
  const auto initialJournal=journal();
  const auto* configured=std::getenv("OPENGOTHIC_PLACEMENT_RECORDS");
  const auto previous=configured ? std::optional<std::string>(configured) : std::nullopt;

  using Notice=std::array<std::string,4>;
  for(int32_t requested:{1,2,3,4,5,6,7}) {
    for(bool nested:{false,true}) {
      if(nested && requested!=4)
        continue;
      const auto run=[&](bool native) {
        setenv("OPENGOTHIC_PLACEMENT_RECORDS",native ? "1" : "0",1);
        Tempest::MemReader reader(initialJournal);
        Serialize in(reader);
        require(in.setEntry("game/quests"),"chapter fixture must save the initial journal");
        script.loadQuests(in);
        auto* rescue=script.findSymbol("TOPIC_RESCUEBENNET");
        vm.push_reference(rescue);
        vm.push_int(int32_t(QuestLog::Status::Running));
        vm.unsafe_call_external(script.findSymbol("LOG_SETTOPICSTATUS"));
        script.findSymbol("MIS_RESCUEBENNET")->set_int(int32_t(QuestLog::Status::Failed));
        script.findSymbol("SEKOB")->set_instance(heroHandle);
        script.findSymbol("BENGAR")->set_instance(pedroHandle);
        hero->handle().flags=NpcFlag::IMMORTAL;
        pedro->handle().flags=NpcFlag::IMMORTAL;
        ambient->set_int(-1);
        alchemy->set_int(0,12);
        alchemy->set_int(9,11);
        vm.unsafe_set_gi(heroHandle);

        std::vector<Notice> notices;
        std::vector<std::string> calls;
        size_t entries=0;
        const auto observer=vm.add_call_observer([&](const auto& symbol) {
          if(symbol.name()=="B_CHECKLOG") {
            require(vm.unsafe_get_gi()==(requested==2 ? pedroHandle : heroHandle),
                    "the journal must inherit the original chapter context");
            calls.push_back(symbol.name());
            }
          if(symbol.name()=="B_ENTER_NEWWORLD" || symbol.name()=="B_ENTER_OLDWORLD")
            calls.push_back(symbol.name());
          });
        vm.observe_instructions([&](const auto&) { if(vm.pc()==root->address()) ++entries; });
        vm.observe_externals([&](const auto& symbol) {
          if(symbol.name()!="INTRODUCECHAPTER")
            return;
          require(std::get<int32_t>(vm.stack_frame(0).value)==6000,"chapter card must last six seconds");
          Notice notice;
          for(uint16_t i=0;i<notice.size();++i) {
            const auto& arg=vm.stack_frame(uint16_t(4-i));
            notice[i]=std::get<daedalus::DaedalusSymbol*>(arg.value)->get_string();
            }
          notices.push_back(std::move(notice));
          });
        const auto pc=vm.pc();
        vm.push_int(0x12345678);
        std::string error;
        const auto result=storygraph::callFunction(vm,*root,{storygraph::Value::integer(requested),
                                                           storygraph::Value::integer(nested ? oldWorld : island)},error);
        const auto sentinel=vm.pop_int();
        vm.observe_externals({});
        vm.observe_instructions({});
        vm.remove_call_observer(observer);

        const auto finalChapter=nested ? 5 : requested;
        const auto finalLevel=nested ? newWorld : island;
        const auto xp=finalChapter<=6 ? script.intSymbol("XP_AMBIENTKAP"+std::to_string(finalChapter),-1) : -1;
        require(result.has_value(),error.c_str());
        require(entries==(native ? 0u : nested ? 2u : 1u) && sentinel==0x12345678 &&
                vm.pc()==pc && vm.unsafe_get_gi()==heroHandle && savedChapter->get_int()==finalChapter &&
                savedLevel->get_int()==finalLevel && chapter->get_int()==finalChapter,
                "chapter switch must preserve the call frame and the final live saved arguments");
        const auto flags=requested==2 ? NpcFlag::NONE : NpcFlag::IMMORTAL;
        require(hero->handle().flags==flags && pedro->handle().flags==flags &&
                alchemy->get_int(12)==(requested>=4 && requested<=6) && alchemy->get_int(11)==9 &&
                ambient->get_int()==xp && notices.size()==size_t(requested<=2),
                "the chapter-specific effects must preserve flags, alchemy, ambient XP and UI");
        const std::vector<std::string> expected=nested ?
          std::vector<std::string>{"B_CHECKLOG","B_ENTER_OLDWORLD","B_CHECKLOG","B_ENTER_NEWWORLD","B_ENTER_NEWWORLD"} :
          std::vector<std::string>{"B_CHECKLOG"};
        require(calls==expected,"chapter switch must update the journal before entering the worlds");
        std::map<std::string,QuestLog::Status> statuses;
        const auto& quests=script.questLog();
        for(size_t i=0;i<quests.questCount();++i) {
          const auto& quest=quests.quest(i);
          statuses.emplace(quest.name,quest.status);
          }
        require(statuses.at(rescue->get_string())==QuestLog::Status::Failed,
                "chapter switch must apply the journal's live mission outcome");
        return std::pair(notices,statuses);
        };
      const auto stock=run(false);
      require(run(true)==stock,"the native chapter switch must use the original UI arguments and update the same quests");
      }
    }
  if(previous) setenv("OPENGOTHIC_PLACEMENT_RECORDS",previous->c_str(),1);
  else unsetenv("OPENGOTHIC_PLACEMENT_RECORDS");
  std::puts("Chapter switch: chapter branches, UI, NPC flags, journal order and nested world dispatch match stock");
  }

void prepareRoutineWitnessWorld(World& world, bool events) {
  ActivityEvents::reset();
  if(!events || world.name()!="ADDONWORLD.ZEN" ||
     NpcSnapshot::load(baseProgramIndex().dir.parent_path()).sha256!=
     "a1918ecb7239b6f48cb98dda5e6ff7eb7ab8099085b3d7adec603c1431de641c")
    return;

  auto& script=world.script();
  auto& vm=script.getVm();
  const auto* init=script.findSymbol("INIT_ADDONWORLD");
  const auto* entry=script.findSymbol("B_ENTER_ADDONWORLD");
  auto* level=script.findSymbol("CURRENTLEVEL");
  auto* chapter=script.findSymbol("KAPITEL");
  auto* jack=script.findSymbol("ALLIGATORJACK");
  const auto boundJack=jack->get_instance();
  const auto addon=script.intSymbol("ADDONWORLD_ZEN",0), khorinis=script.intSymbol("NEWWORLD_ZEN",0);
  require(level->get_int()==addon && boundJack,"first Addon World entry must bind its actors and update the level");
  const auto truth=script.intSymbol("TRUE",1), falsity=script.intSymbol("FALSE",0);
  std::map<std::string_view,int32_t> before;
  for(const auto* name:{"CURRENTLEVEL","KAPITEL","RAVENISDEAD","SKLAVEN_WEG","ENTERADW_KAPITEL2","ENTERADW_KAPITEL3",
                        "ENTERADW_KAPITEL5","ENTERADW_KAPITEL6","ENTERNW_KAPITEL6","ENTEROW_KAPITEL1",
                        "ENTEROW_KAPITEL2","ENTEROW_KAPITEL6"})
    before.emplace(name,script.findSymbol(name)->get_int());
  Environment placement;
  placement.set("OPENGOTHIC_PLACEMENT_RECORDS",std::string("0"));

  using Call=std::pair<std::string,int32_t>;
  const std::vector<Call> expected={
    {"INIT_SUB_ADDON_PART_BANDITSCAMP_01",khorinis},{"INIT_SUB_ADDON_PART_PIRATESCAMP_01",khorinis},
    {"INIT_SUB_ADDON_PART_ENTRANCE_01",khorinis},{"INIT_SUB_ADDON_PART_GOLDMINE_01",khorinis},
    {"INIT_SUB_ADDON_PART_CANYON_01",khorinis},{"INIT_SUB_ADDON_PART_VALLEY_01",khorinis},
    {"INIT_SUB_ADDON_PART_ADANOSTEMPLE_01",khorinis},{"B_INITMONSTERATTITUDES",khorinis},
    {"B_INITGUILDATTITUDES",khorinis},{"B_INITNPCGLOBALS",khorinis},{"B_ENTER_ADDONWORLD",khorinis},
    {"B_INITNPCGLOBALS",khorinis},{"B_ENTER_ADDONWORLD_KAPITEL_1",khorinis},
    {"B_ENTER_ADDONWORLD_KAPITEL_2",khorinis},{"B_ENTER_ADDONWORLD_KAPITEL_3",khorinis},{"B_INITNPCGLOBALS",addon},
    };
  std::vector<uint32_t> migrated{init->address(),entry->address()};
  for(const auto& [name,levelValue]:expected) {
    if(name.starts_with("INIT_SUB_ADDON_PART_") || name=="B_ENTER_ADDONWORLD_KAPITEL_2" || name=="B_ENTER_ADDONWORLD_KAPITEL_3")
      migrated.push_back(script.findSymbol(name)->address());
    }
  const auto none=script.intSymbol("GIL_NONE",-1), publicGuild=script.intSymbol("GIL_PUBLIC",-1);
  const std::vector<Call> expectedRooms={
    {"tavern01",none},{"beds01",publicGuild},{"merchant01",publicGuild},
    {"schmied01",none},{"zoll01",none},{"raven01",publicGuild},
    };
  for(bool native:{false,true}) {
    setenv("OPENGOTHIC_PLACEMENT_RECORDS",native ? "1" : "0",1);
    level->set_int(khorinis);
    chapter->set_int(1);
    jack->set_instance(nullptr);
    for(const auto* name:{"RAVENISDEAD","SKLAVEN_WEG","ENTERADW_KAPITEL2","ENTERADW_KAPITEL3"})
      script.findSymbol(name)->set_int(falsity);
    for(const auto& [room,guild]:expectedRooms)
      world.assignRoomToGuild(room,guild==none ? publicGuild : none);

    std::vector<Call> calls, rooms;
    const auto observer=vm.add_call_observer([&](const auto& symbol) {
      const auto& name=symbol.name();
      if(name.starts_with("INIT_SUB_ADDON_PART_") || name.starts_with("B_ENTER_ADDONWORLD") ||
         name=="B_INITMONSTERATTITUDES" || name=="B_INITGUILDATTITUDES" || name=="B_INITNPCGLOBALS")
        calls.emplace_back(name,level->get_int());
      if(name=="B_ENTER_ADDONWORLD_KAPITEL_1")
        chapter->set_int(3);
      });
    size_t bytecode=0;
    vm.observe_instructions([&](const auto&) {
      if(std::ranges::find(migrated,vm.pc())!=migrated.end()) ++bytecode;
      });
    vm.observe_externals([&](const auto& symbol) {
      if(symbol.name()!="WLD_ASSIGNROOMTOGUILD")
        return;
      const auto* room=std::get<daedalus::DaedalusSymbol*>(vm.stack_frame(1).value);
      const auto* guild=std::get<daedalus::DaedalusSymbol*>(vm.stack_frame(0).value);
      rooms.emplace_back(room->get_string(),guild->get_int());
      });
    const auto pc=vm.pc();
    const auto context=vm.unsafe_get_gi();
    vm.push_int(0x12345678);
    script.runWorldInit("INIT_ADDONWORLD");
    const auto sentinel=vm.pop_int();
    vm.observe_instructions({});
    vm.observe_externals({});
    vm.remove_call_observer(observer);
    require(bytecode==(native ? 0u : migrated.size()) && rooms==expectedRooms && sentinel==0x12345678 &&
            vm.pc()==pc && vm.unsafe_get_gi()==context && calls==expected,
            "Addon World Init must preserve the ordered calls, live chapter and VM context");
    for(const auto& [room,guild]:expectedRooms) {
      std::string sector=room;
      for(auto& ch:sector)
        ch=char(std::toupper(static_cast<unsigned char>(ch)));
      require(world.guildOfRoom("P:"+sector+"_")==guild,"bandit camp must assign the original room owner");
      }
    require(chapter->get_int()==3 && level->get_int()==addon && jack->get_instance()==boundJack &&
            script.intSymbol("ENTERADW_KAPITEL2",0)==truth && script.intSymbol("ENTERADW_KAPITEL3",0)==truth,
            "Addon World must apply the live chapter latches and final NPC aliases");

    static constexpr std::pair<const char*,const char*> latches[]={
      {"B_ENTER_NEWWORLD_KAPITEL_6","ENTERNW_KAPITEL6"},
      {"B_ENTER_OLDWORLD_KAPITEL_1","ENTEROW_KAPITEL1"},
      {"B_ENTER_OLDWORLD_KAPITEL_2","ENTEROW_KAPITEL2"},
      {"B_ENTER_OLDWORLD_KAPITEL_6","ENTEROW_KAPITEL6"},
      {"B_ENTER_ADDONWORLD_KAPITEL_2","ENTERADW_KAPITEL2"},
      {"B_ENTER_ADDONWORLD_KAPITEL_3","ENTERADW_KAPITEL3"},
      {"B_ENTER_ADDONWORLD_KAPITEL_5","ENTERADW_KAPITEL5"},
      {"B_ENTER_ADDONWORLD_KAPITEL_6","ENTERADW_KAPITEL6"},
      };
    for(const auto& [name,flag]:latches) {
      auto* latch=script.findSymbol(flag);
      const auto* function=script.findSymbol(name);
      for(int32_t value:{falsity,truth,2}) {
        latch->set_int(value);
        size_t instructions=0;
        vm.observe_instructions([&](const auto&) { if(vm.pc()==function->address()) ++instructions; });
        vm.unsafe_call_bl(function);
        vm.observe_instructions({});
        require(latch->get_int()==(value==falsity ? truth : value) && instructions==(native ? 0u : 1u),
                "chapter latch must change only the original false value");
        }
      }
    }
  for(const auto& [name,value]:before)
    script.findSymbol(name)->set_int(value);
  std::puts("Addon World entry: Init order, live chapter, room assignments, NPC aliases and eight chapter latches match stock");
  }

} // namespace NpcWorldFixture

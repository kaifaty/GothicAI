#include "npc_definition_fixture.h"
#include "npcdaymeasurement.h"

#include "baseidentity.h"
#include "commandline.h"
#include "game/gamesession.h"
#include "gothic.h"
#include "gamemusic.h"
#include "programdata.h"
#include "resources.h"
#include "world/objects/npc.h"
#include "world/world.h"
#include "worldchunks.h"
#include "worlddata.h"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
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
  if(!NpcDayMeasurement::start(world,directory.string(),0,false,{"NPC_NO_ID"},error)) throw std::runtime_error(error);
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
  if(!NpcDayMeasurement::start(world,directory.string(),0,false,{},error)) throw std::runtime_error(error);
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

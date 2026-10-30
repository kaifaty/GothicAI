#include "worlddata.h"
#include "world/voblayer.h"
#include "basemesh.h"
#include "commandline.h"
#include "resources.h"
#include "graphics/mesh/protomesh.h"
#include "gothic.h"
#include "gamemusic.h"
#include "game/gamesession.h"
#include "game/serialize.h"
#include "world/world.h"
#include "world/movablevobs.h"
#include "utils/workers.h"

#include <Tempest/Log>
#include <Tempest/MemReader>
#include <Tempest/MemWriter>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

namespace fs=std::filesystem;
namespace {
size_t assertions=0;

void require(bool value, const std::string& message) {
  ++assertions;
  if(!value) throw std::runtime_error(message);
  }

void textFile(const fs::path& path, const std::string& text) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path); out << text;
  require(bool(out),"write "+path.string());
  }

int subprocess(const std::vector<std::string>& args) {
  const auto pid=fork();
  require(pid>=0,"subprocess must start");
  if(pid==0) {
    std::vector<char*> argv;
    for(const auto& arg:args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    execvp(argv.front(),argv.data());
    _exit(127);
    }
  int status=0;
  require(waitpid(pid,&status,0)==pid,"subprocess must finish");
  return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
  }

struct Fixture {
  fs::path root,base,content,world,layers;
  Fixture() {
    std::string pattern=(fs::temp_directory_path()/"gothic-vob-layer-XXXXXX").string();
    const auto* dir=mkdtemp(pattern.data());
    require(dir!=nullptr,"temporary fixture directory");
    root=dir; base=root/"base"; content=root/"content";
    world=base/"worlds/TEST"; layers=content/"worlds/TEST/vobs.d";
    auto data=original();
    WorldData::write(data,world);
    fs::create_directories(base/"meshes");
    asset::StaticMesh mesh;
    mesh.positions={{0,0,0},{100,0,0},{0,100,0}};
    mesh.bbox.min={0,0,0}; mesh.bbox.max={100,100,0};
    mesh.sub_meshes.resize(1);
    auto& sub=mesh.sub_meshes[0];
    sub.mat.name="BASE"; sub.mat.group=asset::MaterialGroup::STONE;
    sub.wedges={{{0,0,1},{0,0},0},{{0,0,1},{1,0},1},{{0,0,1},{0,1},2}};
    sub.triangles={{{0,1,2}}};
    writeBaseMesh(mesh,"BASE",base/"meshes/BASE.glb");
    textFile(base/"meshes/meshes.json",R"({"format":"gothic-meshes","version":1,"archives":[],"meshes":{"BASE":"BASE.glb"}})");
    }
  ~Fixture() { std::error_code error; fs::remove_all(root,error); }
  static WorldData::Data original() {
    WorldData::Data data;
    data.source={"TEST.ZEN",1234,42}; data.name="TEST";
    data.waynet=std::make_shared<asset::WayNet>();
    for(uint32_t id:{7u,19u,31u}) {
      auto v=std::make_shared<asset::VirtualObject>();
      v->type=asset::VirtualObjectType::zCVob; v->id=id;
      v->position={float(id),10,20}; v->show_visual=true;
      v->visual_name="BASE.3DS"; v->associated_visual_type=asset::VisualType::MESH;
      v->visual=std::make_shared<asset::VisualMesh>();
      v->visual->type=asset::VisualType::MESH; v->visual->name=v->visual_name;
      data.vobs.push_back(v);
      }
    auto child=std::make_shared<asset::VirtualObject>(*data.vobs[0]);
    child->id=43; data.vobs[0]->children.push_back(child);
    return data;
    }
  void clearLayers() const {
    fs::remove_all(layers); fs::create_directories(layers);
    }
  WorldData::Data read() const {
    auto data=WorldData::read(world,content/"worlds/TEST");
    VobLayer::apply(data,world,content/"worlds/TEST");
    return data;
    }
  };

std::string mesh(const std::string& key, const std::string& visual="BASE.3DS", const std::string& collision="none") {
  return "{\"key\":\""+key+"\",\"kind\":\"mesh\",\"visual\":\""+visual+
         "\",\"collision\":\""+collision+"\",\"position\":[100,200,300],\"yaw_deg\":90}";
  }

std::string emitter(const std::string& key, const std::string& pfx) {
  return "{\"key\":\""+key+"\",\"kind\":\"emitter\",\"pfx\":\""+pfx+
         "\",\"position\":[100,200,300],\"yaw_deg\":90}";
  }

const asset::VirtualObject* find(const WorldData::Data& data, uint32_t id) {
  const auto visit=[&](const auto& self,const auto& list)->const asset::VirtualObject* {
    for(const auto& v:list) {
      if(v->id==id) return v.get();
      if(const auto* child=self(self,v->children)) return child;
      }
    return nullptr;
    };
  return visit(visit,data.vobs);
  }

void noLayers(const Fixture& f) {
  const auto expected=Fixture::original();
  require(WorldData::compare(expected,f.read()).empty(),"no layers must retain every base field, order and id");
  require(WorldData::compare(expected,WorldData::read(f.world)).empty(),"empty content path must retain base");
  setenv("OPENGOTHIC_CONTENT","",1);
  require(Resources::worldOverrideDir("TEST.ZEN").empty(),"empty content disables layers");
  setenv("OPENGOTHIC_CONTENT",f.content.c_str(),1);
  }

void validLayers(const Fixture& f) {
  f.clearLayers();
  textFile(f.layers/"10-add.json","{\"format\":1,\"add\":["+mesh("base-solid","BASE.3DS","mesh")+","+
           mesh("own-ghost","VOB_LAYER_TEST_CUBE","none")+
           R"(,{"key":"lamp","kind":"light","position":[1,2,3],"rotation":[[0,0,1],[0,1,0],[-1,0,0]],"range":450,"color":[255,128,0]}],"remove":[19],"move":[{"id":7,"position":[11,22,33],"yaw_deg":0}]})");
  textFile(f.layers/"20-move.json",R"({"format":1,"move":[{"id":7,"position":[44,55,66],"rotation":[[0,0,1],[0,1,0],[-1,0,0]]},{"id":43,"position":[10,20,30],"yaw_deg":90}]})");
  const auto checked=VobLayer::check(f.world,f.layers/"10-add.json",f.content);
  require(checked.errors.empty() && checked.added==3 && checked.removed==1 && checked.moved==1,"offline checker valid layer counts");
  const auto data=f.read();
  require(data.vobs.size()==5 && WorldData::vobCount(data.vobs)==6,"add three roots and remove one base root");
  require(data.vobs[0]->id==7 && data.vobs[1]->id==31,"untouched base ordering and ids");
  require(find(data,19)==nullptr,"remove must erase base id");
  require(find(data,7)->position.x==44 && find(data,7)->position.y==55,"filename order must determine the last move");
  require(find(data,43)->position.x==10,"move can target nested base id");
  require(data.vobs[2]->cd_static && data.vobs[2]->cd_dynamic &&
          !data.vobs[3]->cd_static && !data.vobs[3]->cd_dynamic,"mesh/none world and player collision flags");
  data.vobs[2]->visual->name="NW_HARBOUR_BARREL_01.3DS";
  require(MovableVobs::classifyVisual(data.vobs[2]->visual->name)!=nullptr &&
          MovableVobs::classify(*data.vobs[2])==nullptr,"layer props never become dynamic movable vobs");
  require(data.vobs[2]->visual_name=="BASE.3DS" && data.vobs[3]->visual_name=="VOB_LAYER_TEST_CUBE.3DS","base and own visuals use runtime loadable names");
  require(std::abs(data.vobs[2]->rotation.columns[0].x)<.0001f &&
          std::abs(std::abs(data.vobs[2]->rotation.columns[0].z)-1.f)<.0001f,"yaw rotates about the vertical axis");
  const auto* light=dynamic_cast<asset::VLight*>(data.vobs[4].get());
  require(light!=nullptr && light->is_static && light->range==450 && light->color.r==255 && light->color.g==128 && light->color.b==0,"static light properties");
  const float rotationRows[3][3]={{0,0,1},{0,1,0},{-1,0,0}};
  for(unsigned row=0;row<3;++row) for(unsigned column=0;column<3;++column)
    require(light->rotation[column][row]==rotationRows[row][column] &&
            find(data,7)->rotation[column][row]==rotationRows[row][column],"add and move preserve vobs.json matrix rows without transposing");
  setenv("OPENGOTHIC_VOB_LAYERS","0",1);
  require(WorldData::compare(Fixture::original(),f.read()).empty(),"OPENGOTHIC_VOB_LAYERS=0 skips all files");
  unsetenv("OPENGOTHIC_VOB_LAYERS");
  require(subprocess({GOTHIC_CONTENT_CONVERT,"check-vob-layer",f.world.string(),(f.layers/"10-add.json").string(),f.content.string()})==0,"converter CLI accepts a valid layer");
  }

void rejectedLayers(const Fixture& f) {
  const std::vector<std::pair<std::string,std::string>> bad={
    {"missing-id",R"({"format":1,"remove":[98765]})"},
    {"missing-move-id",R"({"format":1,"move":[{"id":98765,"position":[0,0,0],"yaw_deg":0}]})"},
    {"duplicate-key","{\"format\":1,\"add\":["+mesh("existing")+"]}"},
    {"duplicate-in-file","{\"format\":1,\"add\":["+mesh("same")+","+mesh("same")+"]}"},
    {"duplicate-remove",R"({"format":1,"remove":[19,19]})"},
    {"unknown-visual","{\"format\":1,\"add\":["+mesh("missing","NO_SUCH_VISUAL")+"]}"},
    {"schema",R"({"format":1,"add":[{"key":"bad-schema","kind":"mesh","position":[0,0],"yaw_deg":0,"visual":"BASE","collision":"none"}]})"},
    {"format",R"({"format":2})"}
    };
  for(const auto& [name,text]:bad) {
    f.clearLayers();
    textFile(f.layers/"00-valid.json","{\"format\":1,\"add\":["+mesh("existing")+"]}");
    // A valid mutation preceding the error must also be rolled back.
    auto transaction=text;
    transaction.insert(1,"\"move\":[{\"id\":31,\"position\":[999,0,0],\"yaw_deg\":0}],");
    if(name=="missing-move-id") transaction=text; // avoid duplicate JSON fields.
    const auto file=f.layers/("10-"+name+".json");
    textFile(file,transaction);
    textFile(f.layers/"20-valid.json","{\"format\":1,\"add\":["+mesh("after-error")+"]}");
    const auto checked=VobLayer::check(f.world,file,f.content);
    require(!checked.errors.empty(),name+": offline validation must reject");
    require(checked.errors.front().find(file.string())!=std::string::npos,name+": error must identify file");
    if(name=="missing-id" || name=="missing-move-id")
      require(checked.errors.front().find("98765")!=std::string::npos,name+": error must identify id");
    if(name=="unknown-visual")
      require(checked.errors.front().find("missing")!=std::string::npos,name+": error must identify key");
    size_t errors=0;
    Tempest::Log::setOutputCallback([&](Tempest::Log::Mode mode,const char* line) {
      if(mode==Tempest::Log::Error && std::string_view(line).find(file.filename().string())!=std::string::npos) ++errors;
      });
    const auto data=f.read();
    Tempest::Log::setOutputCallback({});
    require(errors==1,name+": exactly one error log per rejected file");
    require(data.vobs.size()==5 && find(data,31)->position.x==31,name+": rejected file is atomic and subsequent files still apply");
    if(name=="schema")
      require(subprocess({GOTHIC_CONTENT_CONVERT,"check-vob-layer",f.world.string(),file.string(),f.content.string()})!=0,"converter CLI rejects schema errors");
    }
  }

void removedReferences(const Fixture& f) {
  for(bool move:{false,true}) {
    f.clearLayers();
    textFile(f.layers/"00-remove.json",R"({"format":1,"remove":[19]})");
    const auto file=f.layers/"10-removed-id.json";
    const auto operation=move ? R"("move":[{"id":19,"position":[999,0,0],"yaw_deg":0}])" : R"("remove":[19])";
    textFile(file,"{\"format\":1,\"add\":["+mesh("rolled-back")+"],"+operation+"}");
    textFile(f.layers/"20-valid.json","{\"format\":1,\"add\":["+mesh("after-removed-error")+"]}");
    const auto checked=VobLayer::check(f.world,file,f.content);
    require(!checked.errors.empty() && checked.errors.front().find("19")!=std::string::npos &&
            checked.errors.front().find(file.string())!=std::string::npos,"offline checker rejects an already removed id with file and id");
    size_t errors=0;
    Tempest::Log::setOutputCallback([&](Tempest::Log::Mode mode,const char* line) {
      if(mode==Tempest::Log::Error && std::string_view(line).find(file.filename().string())!=std::string::npos) ++errors;
      });
    const auto data=f.read();
    Tempest::Log::setOutputCallback({});
    require(errors==1 && data.vobs.size()==3 && find(data,19)==nullptr &&
            data.vobs.back()->vob_name=="after-removed-error","removed-id file is skipped atomically and the following file applies");
    }
  }

void unreadableLayers(const Fixture& f) {
  f.clearLayers();
  textFile(f.layers/"add.json","{\"format\":1,\"add\":["+mesh("inaccessible")+"]}");
  auto data=WorldData::read(f.world);
  const auto permissions=fs::status(f.layers).permissions();
  fs::permissions(f.layers,fs::perms::none);
  size_t errors=0;
  Tempest::Log::setOutputCallback([&](Tempest::Log::Mode mode,const char* line) {
    if(mode==Tempest::Log::Error && std::string_view(line).find(f.layers.string())!=std::string::npos) ++errors;
    });
  VobLayer::Result result;
  try { result=VobLayer::apply(data,f.world,f.content/"worlds/TEST"); }
  catch(...) {
    fs::permissions(f.layers,permissions);
    Tempest::Log::setOutputCallback({});
    throw;
    }
  fs::permissions(f.layers,permissions);
  Tempest::Log::setOutputCallback({});
  require(result.errors.size()==1 && errors==1 && result.errors.front().find(f.layers.string())!=std::string::npos,
          "unreadable vobs.d returns one named error and log without throwing");
  require(WorldData::compare(Fixture::original(),data).empty(),"unreadable vobs.d preserves the base world");
  }

void saveBoundPolicy(const Fixture& f) {
  const char* classes[]={
    "zCVob","zCVobLevelCompo","oCItem","oCNpc","zCMoverController","zCVobScreenFX","zCVobStair",
    "zCPFXController","zCVobAnimate","zCVobLensFlare","zCVobLight","zCVobSpot","zCVobStartpoint",
    "zCMessageFilter","zCCodeMaster","zCTriggerWorldStart","zCCSCamera","zCCamTrj_KeyFrame","oCTouchDamage",
    "zCTriggerUntouch","zCEarthquake","oCMOB","oCMobInter","oCMobBed","oCMobFire","oCMobLadder",
    "oCMobSwitch","oCMobWheel","oCMobContainer","oCMobDoor","zCTrigger","zCTriggerList","oCTriggerScript",
    "oCTriggerChangeLevel","oCCSTrigger","zCMover","zCVobSound","zCVobSoundDaytime","oCZoneMusic",
    "oCZoneMusicDefault","zCZoneZFog","zCZoneZFogDefault","zCZoneVobFarPlane","zCZoneVobFarPlaneDefault"
    };
  // zCVob is forbidden only when the shared movable-body classifier selects it.
  // Every other tag is protected, including static-looking save-bound classes.
  Tempest::Log::setOutputCallback([](Tempest::Log::Mode,const char*) {});
  size_t cases=0;
  for(int tag=-1;tag<int(std::size(classes));++tag) {
    const std::string className=tag==-1 ? "UNKNOWN" : classes[size_t(tag)];
    for(bool descendant:{false,true}) for(bool move:{false,true}) {
      f.clearLayers();
      auto data=Fixture::original();
      auto target=std::make_shared<asset::VirtualObject>(*data.vobs[1]);
      target->id=900;
      auto bound=target;
      if(descendant) {
        bound=std::make_shared<asset::VirtualObject>(*target);
        bound->id=901; target->children.push_back(bound);
        }
      bound->type=asset::VirtualObjectType(tag);
      if(tag==0) {
        bound->visual=std::make_shared<asset::VisualMesh>();
        bound->visual->type=asset::VisualType::MESH;
        bound->visual->name="NW_HARBOUR_BARREL_01.3DS";
        require(MovableVobs::classify(*bound)!=nullptr,"policy fixture must match movable physics");
        }
      data.vobs.push_back(target);
      const auto file=f.layers/"10-protected.json";
      const auto operation=move ? R"("move":[{"id":900,"position":[999,0,0],"yaw_deg":0}])" : R"("remove":[900])";
      textFile(file,"{\"format\":1,\"add\":["+mesh("rolled-back")+"],"+operation+"}");
      textFile(f.layers/"20-valid.json","{\"format\":1,\"add\":["+mesh("after-policy-error")+"]}");
      const auto result=VobLayer::apply(data,f.world,f.content/"worlds/TEST");
      require(!result.errors.empty(),className+": remove/move must reject save-bound objects");
      std::string errors;
      for(const auto& error:result.errors) errors+=error;
      require(errors.find(file.string())!=std::string::npos && errors.find(std::to_string(bound->id))!=std::string::npos &&
              errors.find(className)!=std::string::npos,className+": policy error identifies filename, protected id and class");
      require(data.vobs.size()==5 && find(data,900)==target.get() && target->position.x==19 &&
              result.added==1,className+": rejected file rolls back additions and mutation; following file applies");
      ++cases;
      }
    }
  Tempest::Log::setOutputCallback({});
  require(cases==180,"all 44 class tags, unknown and both operations/subtree guards are checked");
  }

void runtimeMesh(const Fixture& f) {
  setenv("OPENGOTHIC_CONTENT",f.content.c_str(),1);
  setenv("OPENGOTHIC_CENSUS","",1);
  const auto game=f.root.string();
  const char* args[]={"gothic-vob-layer-test","-g",game.c_str()};
  CommandLine commandLine(3,args);
  Resources resources;
  const auto* proto=Resources::loadMesh("VOB_LAYER_TEST_CUBE.3DS");
  require(proto!=nullptr && !proto->attach.empty() && proto->attach[0].shape!=nullptr,"own visual without a base counterpart loads through meshes.json and builds collision geometry");
  require(Resources::meshReplacementLoaded("VOB_LAYER_TEST_CUBE.3DS"),"own mesh must be loaded from content");
  }
}

struct WorldDataContract {
  static void meshRegistry(const Fixture& fixture) {
    fixture.clearLayers();
    writeWorldChunks(asset::WorldMesh{},Fixture::original().source,fixture.world);
    const auto manifest=fixture.world/"world.json";
    std::ifstream input(manifest);
    std::string json{std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
    require(!json.empty() && json.front()=='{',"synthetic world manifest");
    json.insert(1,"\"archives\":[],");
    textFile(manifest,json);
    const auto index=fixture.content/"meshes/meshes.json";
    std::ifstream meshes(index);
    const std::string originalIndex{std::istreambuf_iterator<char>(meshes),std::istreambuf_iterator<char>()};
    require(!originalIndex.empty(),"cube mesh registry fixture");
    textFile(index,"{}");
    const auto game=fixture.root.string();
    const char* args[]={"gothic-vob-layer-test","-g",game.c_str()};
    CommandLine commandLine(3,args);
    setenv("OPENGOTHIC_CENSUS","",1);
    const auto file=fixture.layers/"late-mesh.json";
    {
      Resources resources;
      resources.automaticBase.path=fixture.base;
      textFile(index,originalIndex);
      textFile(file,"{\"format\":1,\"add\":["+mesh("late-mesh","VOB_LAYER_TEST_CUBE")+"]}");
      require(VobLayer::check(fixture.world,file,fixture.content).errors.empty(),"offline checker sees the current on-disk mesh registry");
      size_t errors=0;
      Tempest::Log::setOutputCallback([&](Tempest::Log::Mode mode,const char* line) {
        if(mode==Tempest::Log::Error && std::string_view(line).find(file.filename().string())!=std::string::npos) ++errors;
        });
      WorldData::Data data;
      asset::WorldMesh worldMesh;
      Resources::loadWorld(data,worldMesh,"TEST.ZEN");
      Tempest::Log::setOutputCallback({});
      require(errors==1 && WorldData::compare(Fixture::original(),data).empty(),
              "running Resources rejects a late mesh entry atomically using its startup registry");
      }
    {
      Resources resources;
      resources.automaticBase.path=fixture.base;
      WorldData::Data data;
      asset::WorldMesh worldMesh;
      Resources::loadWorld(data,worldMesh,"TEST.ZEN");
      require(data.vobs.size()==4 && data.vobs.back()->vob_name=="late-mesh" && VobLayer::isAdded(*data.vobs.back()),
              "fresh Resources accepts the mesh entry and applies the layer");
      }
    }
  static const Vob* runtimeVob(const Vob& root,uint32_t id) {
    if(root.vobObjectID==id) return &root;
    for(const auto& child:root.child)
      if(const auto* found=runtimeVob(*child,id)) return found;
    return nullptr;
    }
  static const Vob* runtimeVob(const World& world,uint32_t id) {
    for(const auto& root:world.wobj.rootVobs)
      if(const auto* found=runtimeVob(*root,id)) return found;
    return nullptr;
    }
  static size_t layerCount(const Vob& root) {
    size_t count=root.layerObject ? 1u : 0u;
    for(const auto& child:root.child) count+=layerCount(*child);
    return count;
    }
  static size_t layerCount(const World& world) {
    size_t count=0;
    for(const auto& root:world.wobj.rootVobs) count+=layerCount(*root);
    return count;
    }
  static std::vector<uint8_t> save(World& world) {
    std::vector<uint8_t> bytes;
    Tempest::MemWriter writer(bytes);
    { Serialize output(writer); world.save(output); }
    return bytes;
    }
  static void load(World& world,const std::vector<uint8_t>& bytes) {
    Tempest::MemReader reader(bytes);
    Serialize input(reader);
    world.load(input);
    }
  static void saves(const Fixture& fixture) {
    const auto* gameDir=std::getenv("GOTHIC_DIR");
    require(gameDir && *gameDir,"save interchange needs prepared GOTHIC_DIR");
    const char* args[]={"gothic-vob-layer-test","-g",gameDir,"-g2"};
    CommandLine commandLine(4,args);
    setenv("OPENGOTHIC_FRACTURE","",1);
    setenv("OPENGOTHIC_TEXTURES","",1);
    setenv("OPENGOTHIC_PHYSICS_THREADS","0",1);
    Workers::setSerialExecution(true);
    const auto layer=fixture.content/"worlds/NEWWORLD/vobs.d/save-test.json";
    std::vector<uint8_t> baseline,layered;
    uint32_t removed=0,moved=0;
    Tempest::Vec3 originalPosition;
    size_t baseRoots=0;
    for(int phase=0;phase<4;++phase) {
      if(phase==2) setenv("OPENGOTHIC_VOB_LAYERS","0",1);
      if(phase==3) {
        fs::remove(layer);
        unsetenv("OPENGOTHIC_VOB_LAYERS");
        }
      Resources resources;
      Gothic gothic;
      gothic.isNpcInDialogFn=[](const Npc*) { return false; };
      gothic.setupGlobalScripts();
      GameMusic music;
      if(phase==0) {
        const auto data=WorldData::read(Resources::baseDir()/"worlds/NEWWORLD");
        bool foundRemoved=false,foundMoved=false;
        for(const auto& vob:data.vobs) {
          if(vob->type!=asset::VirtualObjectType::zCVob || !vob->children.empty() || MovableVobs::classify(*vob)!=nullptr)
            continue;
          if(!foundRemoved) { removed=vob->id; foundRemoved=true; }
          else if(!foundMoved) {
            moved=vob->id; originalPosition={vob->position.x,vob->position.y,vob->position.z}; foundMoved=true;
            }
          }
        require(foundRemoved && foundMoved,"real base must contain two static leaf vobs outside movable physics");
        }
      gothic.setGame(std::make_unique<GameSession>("NEWWORLD.ZEN"));
      auto& world=*gothic.world();
      if(phase==0) {
        baseRoots=world.wobj.rootVobs.size();
        baseline=save(world);
        require(!baseline.empty(),"save without layer writes an archive");
        textFile(layer,"{\"format\":1,\"remove\":["+std::to_string(removed)+"],\"move\":[{\"id\":"+
                 std::to_string(moved)+",\"position\":[111,222,333],\"yaw_deg\":0}],\"add\":["+
                 mesh("save-cube","VOB_LAYER_TEST_CUBE","mesh")+","+
                 emitter("save-smoke","FIRE_SMOKE.pfx")+","+
                 emitter("save-fire","fire_medium")+","+
                 emitter("save-sparks","Fire_Sparks.PfX")+
                 R"(,{"key":"save-flies","kind":"emitter","pfx":"FLIES","position":[10,20,30],"rotation":[[0,0,1],[0,1,0],[-1,0,0]]},{"key":"save-light","kind":"light","position":[10,20,30],"yaw_deg":0,"range":400,"color":[255,100,20]}]})");
        const auto baseWorld=Resources::baseDir()/"worlds/NEWWORLD";
        const auto checked=VobLayer::check(baseWorld,layer,fixture.content);
        require(checked.errors.empty() && checked.added==6,"offline checker accepts base PFX names in different cases with optional suffix");
        require(subprocess({GOTHIC_CONTENT_CONVERT,"check-vob-layer",baseWorld.string(),layer.string(),fixture.content.string()})==0,
                "converter CLI resolves emitters from base particle programs");
        const auto invalid=layer.parent_path()/"zz-unknown.json";
        textFile(invalid,"{\"format\":1,\"move\":[{\"id\":"+std::to_string(moved)+
                 ",\"position\":[999,0,0],\"yaw_deg\":0}],\"add\":["+
                 emitter("rolled-back","FIRE_SMOKE")+","+emitter("missing-pfx","NO_SUCH_PFX")+"]}");
        const auto rejected=VobLayer::check(baseWorld,invalid,fixture.content);
        require(!rejected.errors.empty() && rejected.errors.front().find("missing-pfx")!=std::string::npos &&
                rejected.errors.front().find(invalid.string())!=std::string::npos,"unknown PFX error names its file and key");
        auto data=WorldData::read(baseWorld);
        const auto applied=VobLayer::apply(data,baseWorld,layer.parent_path().parent_path());
        require(applied.errors.size()==1 && applied.added==6 && find(data,moved)->position.x==111,
                "unknown PFX skips the entire file including its valid emitter and move");
        size_t emitters=0;
        for(const auto& vob:data.vobs) {
          if(!VobLayer::isAdded(*vob) || vob->associated_visual_type!=asset::VisualType::PARTICLE_EFFECT) continue;
          ++emitters;
          require(vob->type==asset::VirtualObjectType::zCVob && vob->show_visual && !vob->cd_static && !vob->cd_dynamic &&
                  vob->visual && vob->visual->type==asset::VisualType::PARTICLE_EFFECT &&
                  (vob->visual_name=="FIRE_SMOKE.PFX" || vob->visual_name=="FIRE_MEDIUM.PFX" ||
                   vob->visual_name=="FIRE_SPARKS.PFX" || vob->visual_name=="FLIES.PFX") && vob->visual->name==vob->visual_name,
                  "layer emitter is a visible, noncolliding native PFX zCVob with a canonical name");
          }
        require(emitters==4,"all four base particle effects use the native emitter visual path");
        fs::remove(invalid);
        }
      else if(phase==1) {
        load(world,baseline);
        require(world.wobj.rootVobs.size()==baseRoots+5 && layerCount(world)==6,"old save loads current mesh/light/emitters exactly once");
        require(runtimeVob(world,removed)==nullptr,"old save cannot resurrect a removed base vob");
        const auto* movedVob=runtimeVob(world,moved);
        require(movedVob && movedVob->position()==Tempest::Vec3(111,222,333),"old save cannot undo an authored layer move");
        layered=save(world);
        load(world,layered);
        require(layerCount(world)==6 && runtimeVob(world,removed)==nullptr,"layered save reload keeps additions once and removal active");
        require(runtimeVob(world,moved)->position()==Tempest::Vec3(111,222,333),"layered save reload keeps current layer move");
        gothic.clearGame().reset();
        gothic.setGame(std::make_unique<GameSession>("NEWWORLD.ZEN"));
        auto& reloaded=*gothic.world();
        require(reloaded.wobj.rootVobs.size()==baseRoots+5 && layerCount(reloaded)==6,
                "recreating the world destroys the previous layer and creates its emitters once");
        load(reloaded,layered);
        require(layerCount(reloaded)==6,"loading a layered save into a recreated world does not duplicate emitters");
        gothic.clearGame().reset();
        gothic.setGame(std::make_unique<GameSession>("OLDWORLD.ZEN"));
        require(layerCount(*gothic.world())==0,"loading a different world leaves no emitters from the previous world");
        gothic.clearGame().reset();
        gothic.setGame(std::make_unique<GameSession>("NEWWORLD.ZEN"));
        require(layerCount(*gothic.world())==6,"returning to the layered world creates its additions exactly once");
        }
      else {
        load(world,layered);
        require(world.wobj.rootVobs.size()==baseRoots && layerCount(world)==0,"layered save loads without the layer or extra objects");
        const auto* movedVob=runtimeVob(world,moved);
        require(movedVob && movedVob->position()==originalPosition,"layer move is not baked into a layer-free save load");
        require(runtimeVob(world,removed)!=nullptr,"disabled layer restores the base tree when reading a layered save");
        }
      gothic.clearGame().reset();
      }
    }
  };

int main() {
  try {
    require(std::string_view(GOTHIC_BASE_FORMAT_SHA256)=="ebeeae0437d4372d7f04fdf7b6915f56b80d00e24c5e8922486e0928e293e5f3",
            "vob layers must retain the existing base format SHA-256");
    Fixture fixture;
    setenv("OPENGOTHIC_CONTENT",fixture.content.c_str(),1);
    unsetenv("OPENGOTHIC_VOB_LAYERS");
    require(subprocess({"python3",GOTHIC_VOB_FIXTURE_SCRIPT,"--content",fixture.content.string()})==0,"cube fixture generator");
    fs::create_directories(fixture.layers);
    noLayers(fixture);
    validLayers(fixture);
    rejectedLayers(fixture);
    removedReferences(fixture);
    unreadableLayers(fixture);
    saveBoundPolicy(fixture);
    WorldDataContract::meshRegistry(fixture);
    WorldDataContract::saves(fixture);
    runtimeMesh(fixture);
    std::cout << "{\"test\":\"vob_layer\",\"passed\":true,\"assertions\":" << assertions << "}\n";
    return 0;
    }
  catch(const std::exception& error) {
    Tempest::Log::setOutputCallback({});
    std::cerr << "vob layer test failed: " << error.what() << '\n';
    return 1;
    }
  }

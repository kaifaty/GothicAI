#include "worlddata.h"
#include "world/voblayer.h"
#include "basemesh.h"
#include "commandline.h"
#include "resources.h"
#include "worldbase.h"
#include "graphics/mesh/protomesh.h"
#include "gothic.h"
#include "gamemusic.h"
#include "game/gamesession.h"
#include "game/serialize.h"
#include "game/definitions/particlecontent.h"
#include "graphics/pfx/particlefx.h"
#include "graphics/pfx/pfxmotion.h"
#include "graphics/lightsource.h"
#include "world/world.h"
#include "world/movablevobs.h"
#include "world/objects/vobbundle.h"
#include "utils/workers.h"

#include <Tempest/Log>
#include <Tempest/MemReader>
#include <Tempest/MemWriter>
#include <algorithm>
#include <chrono>
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
           R"(,{"key":"lamp","kind":"light","position":[1,2,3],"rotation":[[0,0,1],[0,1,0],[-1,0,0]],"range":450,"color":[255,128,0],"color_ani":[[255,128,0],[200,90,10],[240,120,30]],"range_ani":[1,0.85],"ani_fps":12,"cookie":"own_lamp_cookie.tga","source_cm":1.5}],"remove":[19],"move":[{"id":7,"position":[11,22,33],"yaw_deg":0}]})");
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
  require(light->color_animation_list.size()==3 && light->color_animation_list[1].r==200 && light->color_animation_list[1].b==10 &&
          light->color_animation_fps==12 && light->range_animation_scale.size()==2 && light->range_animation_scale[1]==0.85f &&
          light->range_animation_fps==12,"light flicker keys");
  require(light->cookie=="OWN_LAMP_COOKIE.TGA","light mask name, upper case");
  require(VobLayer::lightSourceCm(*light)==1.5f,"light source size");
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
    {"light-ani-fps",R"({"format":1,"add":[{"key":"bad-flicker","kind":"light","position":[0,0,0],"yaw_deg":0,"range":100,"color":[1,2,3],"color_ani":[[1,2,3],[4,5,6]]}]})"},
    {"light-ani-scale",R"({"format":1,"add":[{"key":"bad-flicker","kind":"light","position":[0,0,0],"yaw_deg":0,"range":100,"color":[1,2,3],"range_ani":[1,5],"ani_fps":10}]})"},
    {"light-cookie",R"({"format":1,"add":[{"key":"bad-cookie","kind":"light","position":[0,0,0],"yaw_deg":0,"range":100,"color":[1,2,3],"cookie":"masks/LAMP.TGA"}]})"},
    {"light-source",R"({"format":1,"add":[{"key":"bad-source","kind":"light","position":[0,0,0],"yaw_deg":0,"range":100,"color":[1,2,3],"source_cm":0}]})"},
    {"light-ani-alone",R"({"format":1,"add":[{"key":"bad-flicker","kind":"light","position":[0,0,0],"yaw_deg":0,"range":100,"color":[1,2,3],"ani_fps":10}]})"},
    {"light-flicker-from",R"({"format":1,"add":[{"key":"bad-flicker","kind":"light","position":[0,0,0],"yaw_deg":0,"range":100,"color":[1,2,3],"flicker_from":"no-such-flame"}]})"},
    {"light-jitter-alone",R"({"format":1,"add":[{"key":"bad-flicker","kind":"light","position":[0,0,0],"yaw_deg":0,"range":100,"color":[1,2,3],"jitter_cm":1}]})"},
    {"light-flicker-ani",R"({"format":1,"add":[{"key":"bad-flicker","kind":"light","position":[0,0,0],"yaw_deg":0,"range":100,"color":[1,2,3],"flicker_from":"x","color_ani":[[1,2,3],[4,5,6]],"ani_fps":10}]})"},
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

void particleFields(daedalus::DaedalusVm& vm, const Def::ParticleEffect& actual,
                    const Def::ParticleEffect& expected) {
  const auto* cls=vm.find_symbol_by_name("C_PARTICLEFX");
  require(cls!=nullptr,"particle class exists in base program");
  const auto fields=vm.find_class_members(*cls);
  require(fields.size()==57,"every original C_PARTICLEFX field is compared");
  size_t compared=0;
  for(const auto* field:fields) {
#define COMPARE_PARTICLE_FIELD(type, fieldName, extent, symbol) \
    if(field->name()=="C_PARTICLEFX." symbol) { \
      require(actual.fieldName==expected.fieldName,"inherited/set source particle field "+field->name()); \
      ++compared; \
      continue; \
      }
    DEF_PARTICLE_EFFECT_FIELDS(COMPARE_PARTICLE_FIELD)
#undef COMPARE_PARTICLE_FIELD
    throw std::runtime_error("unexpected particle field "+field->name());
    }
  require(compared==57,"every original C_PARTICLEFX field was compared");
  }

void particleSettings(const ParticleFx& actual, const ParticleFx& expected) {
#define SAME_PARTICLE_FIELD(field) require(actual.field==expected.field,"derived particle setting " #field)
  SAME_PARTICLE_FIELD(ppsValue); SAME_PARTICLE_FIELD(ppsScaleKeys);
  SAME_PARTICLE_FIELD(ppsIsLooping); SAME_PARTICLE_FIELD(ppsIsSmooth); SAME_PARTICLE_FIELD(ppsFPS);
  SAME_PARTICLE_FIELD(ppsCreateEm); SAME_PARTICLE_FIELD(ppsCreateEmDelay);
  SAME_PARTICLE_FIELD(shpType); SAME_PARTICLE_FIELD(shpFOR); SAME_PARTICLE_FIELD(shpOffsetVec);
  SAME_PARTICLE_FIELD(shpDistribType); SAME_PARTICLE_FIELD(shpDistribWalkSpeed); SAME_PARTICLE_FIELD(shpIsVolume);
  SAME_PARTICLE_FIELD(shpDim); SAME_PARTICLE_FIELD(shpMesh); SAME_PARTICLE_FIELD(shpMesh_S); SAME_PARTICLE_FIELD(shpMeshRender);
  SAME_PARTICLE_FIELD(shpScaleKeys); SAME_PARTICLE_FIELD(shpScaleIsLooping); SAME_PARTICLE_FIELD(shpScaleIsSmooth);
  SAME_PARTICLE_FIELD(shpScaleFPS); SAME_PARTICLE_FIELD(dirMode); SAME_PARTICLE_FIELD(dirFOR);
  SAME_PARTICLE_FIELD(dirModeTargetFOR); SAME_PARTICLE_FIELD(dirModeTargetPos);
  SAME_PARTICLE_FIELD(dirAngleHead); SAME_PARTICLE_FIELD(dirAngleHeadVar); SAME_PARTICLE_FIELD(dirAngleElev);
  SAME_PARTICLE_FIELD(dirAngleElevVar); SAME_PARTICLE_FIELD(velAvg); SAME_PARTICLE_FIELD(velVar);
  SAME_PARTICLE_FIELD(lspPartAvg); SAME_PARTICLE_FIELD(lspPartVar); SAME_PARTICLE_FIELD(flyGravity); SAME_PARTICLE_FIELD(flyCollDet);
  SAME_PARTICLE_FIELD(visMaterial); SAME_PARTICLE_FIELD(visOrientation); SAME_PARTICLE_FIELD(visTexIsQuadPoly);
  SAME_PARTICLE_FIELD(visTexAniFPS); SAME_PARTICLE_FIELD(visTexAniIsLooping); SAME_PARTICLE_FIELD(visTexColorStart);
  SAME_PARTICLE_FIELD(visTexColorEnd); SAME_PARTICLE_FIELD(visSizeStart); SAME_PARTICLE_FIELD(visSizeEndScale);
  SAME_PARTICLE_FIELD(visAlphaStart); SAME_PARTICLE_FIELD(visAlphaEnd); SAME_PARTICLE_FIELD(visYawAlign); SAME_PARTICLE_FIELD(visZBias);
  SAME_PARTICLE_FIELD(trlTexture); SAME_PARTICLE_FIELD(trlFadeSpeed); SAME_PARTICLE_FIELD(trlWidth);
  SAME_PARTICLE_FIELD(mrkTexture); SAME_PARTICLE_FIELD(mrkFadeSpeed); SAME_PARTICLE_FIELD(mrkSize);
  SAME_PARTICLE_FIELD(flockMode); SAME_PARTICLE_FIELD(flockStrength); SAME_PARTICLE_FIELD(useEmittersFOR);
  SAME_PARTICLE_FIELD(timeStartEnd_S); SAME_PARTICLE_FIELD(m_bIsAmbientPFX); SAME_PARTICLE_FIELD(prefferedTime);
#undef SAME_PARTICLE_FIELD
  require(actual.maxLifetime()==expected.maxLifetime() && actual.maxPps()==expected.maxPps() &&
          actual.effectPrefferedTime()==expected.effectPrefferedTime(),"derived lifetime and emission limits");
  }

void particleDefinitions(const Fixture& f, const fs::path& baseRoot) {
  const auto directory=f.content/"particles.d";
  textFile(directory/"10-smoke.json",R"({"format":1,"effects":[
    {"name":"OWN_TEST_COPY","base":"fire_smoke.pfx","set":{}},
    {"name":"OWN_TEST_SMOKE","base":"FIRE_SMOKE","set":{"visTexColorStart_S":"80 80 80","visAlphaStart":200,"lspPartAvg":12000,"ppsIsSmooth":1}},
    {"name":"OWN_TEST_CHAIN","base":"own_test_smoke.pfx","set":{"velAvg":0.23}}]})");
  textFile(directory/"20-chain.json",R"({"format":1,"effects":[{"name":"OWN_TEST_NEXT","base":"own_test_chain.PfX","set":{}}]})");
  textFile(directory/"30-flip.json",R"({"format":1,"effects":[
    {"name":"OWN_TEST_FLIP","base":"FIRE_SMOKE","set":{},"flipbook":{"cols":8,"rows":4,"frames":30,"fps":24,"yaw":true,"steady":true}},
    {"name":"OWN_TEST_FLIP_CHILD","base":"OWN_TEST_FLIP","set":{"velAvg":0.1}}]})");
  textFile(directory/"40-motion.json",R"({"format":1,"effects":[
    {"name":"OWN_TEST_SPARKS","base":"FIRE_SPARKS","set":{"visOrientation_S":"VELO"},"curl":{"strength":1.5,"frequency":4,"speed":0.3},"drag":2,"stretch":0.025,"minPixels":1.5},
    {"name":"OWN_TEST_SPARKS_CHILD","base":"OWN_TEST_SPARKS","set":{},"drag":3}]})");
  auto vm=ParticleContent::openProgram(baseRoot);
  const auto original=ParticleContent::base(*vm,"FIRE_SMOKE");
  require(original!=nullptr,"base FIRE_SMOKE definition");
  const auto loaded=ParticleContent::load(*vm,f.content);
  require(loaded.errors.empty() && loaded.effects.size()==8,"own definitions load in filename and declaration order");
  const auto& sparks=loaded.motions.at("OWN_TEST_SPARKS_CHILD");
  require(sparks.curlStrength==1.5f && sparks.curlFrequency==4.f && sparks.curlSpeed==0.3f && sparks.drag==3.f &&
          sparks.stretch==0.025f && sparks.minPixels==1.5f && loaded.motions.at("OWN_TEST_SPARKS").drag==2.f,
          "curl, drag, stretch and minPixels are read and inherited field by field from an own base");
  require(loaded.motions.count("OWN_TEST_SMOKE")==0 && loaded.motions.count("OWN_TEST_FLIP")==0,"effects without them have no motion");
  const auto flip=loaded.flipbooks.find("OWN_TEST_FLIP");
  require(flip!=loaded.flipbooks.end() && flip->second.cols==8 && flip->second.rows==4 && flip->second.frames==30 &&
          flip->second.fps==24.f && flip->second.yaw && flip->second.steady,"flipbook fields are read");
  require(loaded.flipbooks.count("OWN_TEST_FLIP_CHILD")==1 && loaded.flipbooks.at("OWN_TEST_FLIP_CHILD").frames==30 &&
          loaded.flipbooks.count("OWN_TEST_SMOKE")==0,"a flipbook is inherited from an own base only");
  particleFields(*vm,*loaded.effects.at("OWN_TEST_COPY"),*original);
  auto expected=*original;
  expected.vis_tex_color_start_s="80 80 80"; expected.vis_alpha_start=200;
  expected.lsp_part_avg=12000; expected.pps_is_smooth=1;
  particleFields(*vm,*loaded.effects.at("OWN_TEST_SMOKE"),expected);
  expected.vel_avg=0.23f;
  particleFields(*vm,*loaded.effects.at("OWN_TEST_CHAIN"),expected);
  particleFields(*vm,*loaded.effects.at("OWN_TEST_NEXT"),expected);
  require(subprocess({GOTHIC_CONTENT_CONVERT,"check-particles",baseRoot.string(),f.content.string()})==0,
          "converter CLI accepts own definitions and chains");

  const std::vector<std::pair<std::string,std::string>> invalid={
    {"unknown-field",R"({"name":"OWN_BAD","base":"FIRE_SMOKE","set":{"unknownField":1}})"},
    {"float-type",R"({"name":"OWN_BAD","base":"FIRE_SMOKE","set":{"visAlphaStart":"opaque"}})"},
    {"int-type",R"({"name":"OWN_BAD","base":"FIRE_SMOKE","set":{"ppsIsSmooth":0.5}})"},
    {"string-type",R"({"name":"OWN_BAD","base":"FIRE_SMOKE","set":{"visTexColorStart_S":80}})"},
    {"unknown-base",R"({"name":"OWN_BAD","base":"NO_SUCH_BASE","set":{}})"},
    {"base-conflict",R"({"name":"FIRE_SMOKE","base":"LIGHTSMOKE","set":{}})"},
    {"duplicate",R"({"name":"OWN_PREVIOUS","base":"FIRE_SMOKE","set":{}})"},
    {"format",R"({"name":"OWN_BAD","base":"FIRE_SMOKE","set":{}})"},
    {"name",R"({"name":"own-smoke","base":"FIRE_SMOKE","set":{}})"},
    {"flip-frames",R"({"name":"OWN_BAD","base":"FIRE_SMOKE","set":{},"flipbook":{"cols":2,"rows":2,"frames":5,"fps":24}})"},
    {"flip-grid",R"({"name":"OWN_BAD","base":"FIRE_SMOKE","set":{},"flipbook":{"cols":9,"rows":1,"frames":4,"fps":24}})"},
    {"flip-field",R"({"name":"OWN_BAD","base":"FIRE_SMOKE","set":{},"flipbook":{"cols":2,"rows":2,"frames":4,"fps":24,"loop":1}})"},
    {"flip-yaw",R"({"name":"OWN_BAD","base":"FIRE_SMOKE","set":{},"flipbook":{"cols":2,"rows":2,"frames":4,"fps":24,"yaw":1}})"},
    {"flip-steady",R"({"name":"OWN_BAD","base":"FIRE_SMOKE","set":{},"flipbook":{"cols":2,"rows":2,"frames":4,"fps":24,"steady":1}})"},
    {"curl-frequency",R"({"name":"OWN_BAD","base":"FIRE_SMOKE","set":{},"curl":{"strength":1,"frequency":0}})"},
    {"curl-field",R"({"name":"OWN_BAD","base":"FIRE_SMOKE","set":{},"curl":{"strength":1,"frequency":2,"octaves":3}})"},
    {"drag-range",R"({"name":"OWN_BAD","base":"FIRE_SMOKE","set":{},"drag":60})"},
    {"stretch-orientation",R"({"name":"OWN_BAD","base":"FIRE_SMOKE","set":{"visOrientation_S":"NONE"},"stretch":0.02})"},
    {"min-pixels-flipbook",R"({"name":"OWN_BAD","base":"FIRE_SMOKE","set":{},"minPixels":1.5,"flipbook":{"cols":2,"rows":2,"frames":4,"fps":24}})"}
    };
  const auto errorsRoot=f.root/"particle-errors";
  for(const auto& [kind,entry]:invalid) {
    fs::remove_all(errorsRoot);
    textFile(errorsRoot/"particles.d/00-valid.json",R"({"format":1,"effects":[{"name":"OWN_PREVIOUS","base":"FIRE_SMOKE","set":{}}]})");
    const auto file=errorsRoot/"particles.d"/("10-"+kind+".json");
    textFile(file,"{\"format\":"+std::string(kind=="format" ? "2" : "1")+
             R"(,"effects":[{"name":"OWN_ROLLED_BACK","base":"OWN_PREVIOUS","set":{}},)"+entry+"]}");
    textFile(errorsRoot/"particles.d/20-valid.json",R"({"format":1,"effects":[{"name":"OWN_AFTER_ERROR","base":"OWN_PREVIOUS","set":{}}]})");
    const auto checked=ParticleContent::check(baseRoot,errorsRoot);
    require(checked.errors.size()==1 && checked.errors.front().find(file.string())!=std::string::npos,
            kind+": error identifies the rejected particle file");
    require(checked.effects.size()==2 && checked.effects.count("OWN_PREVIOUS")==1 &&
            checked.effects.count("OWN_AFTER_ERROR")==1 && checked.effects.count("OWN_ROLLED_BACK")==0,
            kind+": rejected file is atomic and subsequent files still load");
    if(kind=="unknown-field")
      require(checked.errors.front().find("unknownField")!=std::string::npos &&
              subprocess({GOTHIC_CONTENT_CONVERT,"check-particles",baseRoot.string(),errorsRoot.string()})!=0,
              "converter CLI names and rejects an unknown particle field");
    }
  }

void runtimeParticles(Gothic& gothic, const Fixture& f, const fs::path& baseRoot) {
  const auto* original=gothic.loadParticleFx("FIRE_SMOKE");
  const auto* copy=gothic.loadParticleFx("own_test_copy.pfx");
  const auto* custom=gothic.loadParticleFx("own_test_smoke.PfX");
  require(original && copy && custom,"Gothic resolves base and custom names case-insensitively with suffix");
  require(custom==gothic.loadParticleFx("OWN_TEST_SMOKE"),"custom aliases reuse one cached ParticleFx");
  particleSettings(*copy,*original);
  auto vm=ParticleContent::openProgram(baseRoot);
  const auto definitions=ParticleContent::load(*vm,f.content);
  ParticleFx expected(*definitions.effects.at("OWN_TEST_SMOKE"),"expected");
  particleSettings(*custom,expected);
  require(custom->visTexColorStart==Tempest::Vec3(80,80,80) && custom->visAlphaStart==200.f/255.f &&
          custom->lspPartAvg==12000 && custom->ppsIsSmooth,"custom settings reach the derived native effect");
  const auto* chain=gothic.loadParticleFx("OWN_TEST_NEXT");
  require(chain && chain->velAvg==0.23f && chain->lspPartAvg==12000 &&
          chain->visTexColorStart==custom->visTexColorStart,"runtime resolves chains across particle files");
  const auto* flip=gothic.loadParticleFx("OWN_TEST_FLIP_CHILD");
  require(flip && flip->flipCols==8 && flip->flipRows==4 && flip->flipFrames==30 && flip->flipFps==24.f &&
          flip->visYawAlign && flip->flipSteady && flip->velAvg==0.1f,"an inherited flipbook reaches the native effect, upright cards");
  require(custom->flipFrames==0 && !custom->visYawAlign && !custom->flipSteady,"effects without a flipbook keep the sprite");
  const auto* sparks=gothic.loadParticleFx("OWN_TEST_SPARKS_CHILD");
  require(sparks && std::abs(sparks->motion.curlStrength-1.5e-4f)<1e-9f && std::abs(sparks->motion.curlWave-0.04f*6.2831853f)<1e-6f &&
          std::abs(sparks->motion.curlDrift-0.03f)<1e-7f && std::abs(sparks->motion.drag-3e-3f)<1e-8f &&
          std::abs(sparks->stretchMs-25.f)<1e-4f && sparks->minPixels==1.5f &&
          sparks->visOrientation==ParticleFx::Orientation::Velocity,"own motion reaches the native effect in cm and ms");
  require(!custom->motion.active() && custom->stretchMs==0 && custom->minPixels==0 &&
          !original->motion.active(),"base effects and effects without own motion keep the native path");
  }


// Own curl and drag (pfxmotion.h): the mean path of a cloud of sparks does not depend on the frame rate from 15 to
// 144 fps by more than 5 % of its length; the cost of 2000 particles a frame at 60 fps goes to the log
void particleMotion() {
  PfxMotion::Params p;
  p.curlStrength=1.5e-4f;                  // 1.5 m/s^2
  p.curlWave=4.f*6.2831853f/100.f;         // 4 periods a metre
  p.curlDrift=0.03f;                       // 0.3 m/s
  p.drag=2e-3f;                            // 2 /s
  const Tempest::Vec3 gravity(0,-1e-4f,0); // 1 m/s^2
  const Tempest::Vec3 origin(420,94,-3086);
  struct Particle { Tempest::Vec3 pos, vel; };
  auto start=[](size_t n) {
    std::vector<Particle> ps(n);
    uint32_t r=1;
    auto rnd=[&] { r=r*1664525u+1013904223u; return float(r>>8)/float(1u<<24)*2.f-1.f; };
    for(auto& q:ps) {
      q.pos=Tempest::Vec3(rnd()*5,rnd()*5,rnd()*5);
      q.vel=Tempest::Vec3(rnd()*0.05f,0.15f+rnd()*0.05f,rnd()*0.05f); // 1.5 m/s up
      }
    return ps;
    };
  auto run=[&](double fps, uint64_t until, size_t n) {
    auto ps=start(n);
    uint64_t last=0;
    for(int frame=1; last<until; ++frame) {
      const uint64_t now=std::min(until,uint64_t(double(frame)*1000.0/fps));
      for(auto& q:ps) PfxMotion::step(p,gravity,q.pos,q.vel,origin,float(last),float(now-last));
      last=now;
      }
    return ps;
    };
  auto mean=[](const std::vector<Particle>& ps) {
    Tempest::Vec3 m;
    for(auto& q:ps) m+=q.pos;
    return m/float(ps.size());
    };
  const auto m0=mean(start(500));
  for(uint64_t until:{300u,600u,1000u}) {
    const auto reference=mean(run(1000,until,500));
    for(double fps:{15.0,30.0,60.0,144.0}) {
      const auto m=mean(run(fps,until,500));
      const float error=(m-reference).length()/(reference-m0).length();
      require(error<0.05f,"curl and drag: the mean path at "+std::to_string(int(fps))+" fps, "+std::to_string(until)+
              " ms differs from 1 ms steps by "+std::to_string(error*100.f)+" %");
      }
    }
  p.drag=0;
  require((run(15,1000,1)[0].pos-run(1000,1000,1)[0].pos).length()<
          0.05f*(run(1000,1000,1)[0].pos-start(1)[0].pos).length(),"curl without drag keeps the path at 15 fps");
  // a divergence-free field: the flux through a small cube is zero
  const float h=0.5f;
  const Tempest::Vec3 c(37,-12,105);
  const float flux=(PfxMotion::curl(p,c+Tempest::Vec3(h,0,0),0).x-PfxMotion::curl(p,c-Tempest::Vec3(h,0,0),0).x)+
                   (PfxMotion::curl(p,c+Tempest::Vec3(0,h,0),0).y-PfxMotion::curl(p,c-Tempest::Vec3(0,h,0),0).y)+
                   (PfxMotion::curl(p,c+Tempest::Vec3(0,0,h),0).z-PfxMotion::curl(p,c-Tempest::Vec3(0,0,h),0).z);
  require(std::abs(flux)<1e-3f*p.curlStrength,"the curl field is divergence-free");

  p.drag=2e-3f;
  auto ps=start(2000);
  double best=1e9;
  for(int round=0;round<20;++round) {
    const auto t0=std::chrono::steady_clock::now();
    for(auto& q:ps) PfxMotion::step(p,gravity,q.pos,q.vel,origin,float(round*17),17.f);
    best=std::min(best,std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t0).count());
    }
  std::cout << "{\"test\":\"pfx_motion\",\"particles\":2000,\"frame_ms\":17,\"cpu_ms\":" << best << "}\n";

  // the flame's signal drives a lamp: its brightness is the emitter's signal at the same world tick, without the
  // light's own time offset, and its position trembles within jitter_cm
  PfxMotion::Flicker flicker{6.f,12.f,0.2f,7u};
  LightSource light;
  light.setColor(Tempest::Vec3(1.f,0.5f,0.2f));
  light.setPosition(Tempest::Vec3(10,20,30));
  light.setTimeOffset(1234);
  light.setFlicker(flicker,2.f);
  require(light.isDynamic(),"a flickering light animates");
  float lo=2, hi=0;
  for(uint64_t t=0;t<5000;t+=7) {
    light.update(t);
    const float s=PfxMotion::flicker(flicker,t);
    lo=std::min(lo,s); hi=std::max(hi,s);
    require(light.currentColor()==Tempest::Vec3(1.f,0.5f,0.2f)*s,"the light's brightness is the flame's signal");
    const auto d=light.currentPosition()-Tempest::Vec3(10,20,30);
    require(std::abs(d.x)<=2.f && std::abs(d.y)<=2.f && std::abs(d.z)<=2.f,"the light trembles within jitter_cm");
    }
  require(lo>=0.8f && hi<=1.2f && hi-lo>0.2f,"the signal swings within 1 +- amount");
  }

// A layer emitter's flame signal ("flicker") and the light that takes it ("flicker_from", "jitter_cm")
void flameLayer(const Fixture& f, const fs::path& baseRoot) {
  const auto file=f.root/"flame/00-flame.json";
  const std::string flame=R"({"key":"flame","kind":"emitter","pfx":"own_test_sparks","position":[420,94,-3086],"yaw_deg":0,)"
                          R"("flicker":{"pulse_hz":6,"jitter_hz":12,"amount":0.2,"seed":7}})";
  textFile(file,"{\"format\":1,\"add\":["+flame+R"(,{"key":"flame-light","kind":"light","position":[420,110,-3070],"yaw_deg":0,)"
                R"("range":400,"color":[255,150,60],"flicker_from":"flame","jitter_cm":2}]})");
  Resources::VobTree tree;
  const auto checked=VobLayer::readTree(tree,baseRoot,file,f.content);
  require(checked.errors.empty() && tree.size()==2,"an emitter with flicker and a light with flicker_from are accepted");
  float jitter=-1;
  const auto* emitterSignal=VobLayer::flicker(*tree[0]);
  const auto* lightSignal=VobLayer::flicker(*tree[1],&jitter);
  require(emitterSignal && lightSignal && emitterSignal->pulseHz==6.f && emitterSignal->jitterHz==12.f &&
          emitterSignal->amount==0.2f && emitterSignal->seed==7 && lightSignal->seed==7 && lightSignal->amount==0.2f &&
          jitter==2.f,"the light takes its emitter's signal and its own trembling");
  const std::vector<std::pair<std::string,std::string>> bad={
    {"amount",R"({"key":"flame","kind":"emitter","pfx":"FIRE_SMOKE","position":[0,0,0],"yaw_deg":0,"flicker":{"pulse_hz":6,"jitter_hz":12,"amount":0,"seed":7}})"},
    {"field",R"({"key":"flame","kind":"emitter","pfx":"FIRE_SMOKE","position":[0,0,0],"yaw_deg":0,"flicker":{"pulse_hz":6,"jitter_hz":12,"amount":0.2,"seed":7,"phase":1}})"},
    {"quiet",R"({"key":"flame","kind":"emitter","pfx":"FIRE_SMOKE","position":[0,0,0],"yaw_deg":0})"}
    };
  for(const auto& [reason,entry]:bad) {
    textFile(file,"{\"format\":1,\"add\":["+entry+R"(,{"key":"flame-light","kind":"light","position":[0,0,0],"yaw_deg":0,)"
                  R"("range":400,"color":[255,150,60],"flicker_from":"flame"}]})");
    Resources::VobTree rejected;
    const auto result=VobLayer::readTree(rejected,baseRoot,file,f.content);
    require(!result.errors.empty() && rejected.empty(),"flicker "+reason+": the file is rejected");
    }
  fs::remove_all(file.parent_path());
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
  static void vobTrees(const Fixture& fixture, Resources& resources, World& world) {
    const auto baseRoot=Resources::baseDir();
    const auto file=fixture.content/"vobtrees.d/fIrEtReE_lAmP.json";
    WorldData::ZenFile base;
    require(Resources::loadBaseZen("FIRETREE_LAMP.ZEN","world",base) && !base.vobs.empty(),
            "real base contains the lamp fire bundle");
    WorldData::Data expected;
    expected.vobs=base.vobs;
    const auto fallback=[&](const std::string& context) {
      WorldData::Data actual;
      const auto* tree=loadVobBundle("FIRETREE_LAMP.ZEN");
      require(tree!=nullptr,context+": bundle is available");
      actual.vobs=*tree;
      require(WorldData::compare(expected,actual).empty(),context+": every base bundle field is preserved");
      return tree;
      };
    const auto* missing=fallback("missing tree");
    require(loadVobBundle("FIRETREE_LAMP.ZEN")==missing,"base bundle is cached");
    const auto valid="{\"format\":1,\"add\":["+emitter("own-flame","own_test_smoke.pfx")+","+
                     emitter("base-flame","Fire_Medium.PfX")+","+mesh("slot-mesh","VOB_LAYER_TEST_CUBE")+
                     R"(,{"key":"slot-light","kind":"light","position":[1,2,3],"rotation":[[0,0,1],[0,1,0],[-1,0,0]],"range":450,"color":[255,128,0],"color_ani":[[255,128,0],[200,90,10]],"range_ani":[1,0.85],"ani_fps":12}]})";
    textFile(file,valid);
    Resources::VobTree checkedTree;
    const auto checked=VobLayer::readTree(checkedTree,baseRoot,file,fixture.content);
    require(checked.errors.empty() && checked.added==4 && checkedTree.size()==4,
            "offline tree validation accepts the shared emitter, mesh and light entries");
    require(subprocess({GOTHIC_CONTENT_CONVERT,"check-vob-tree",baseRoot.string(),file.string(),fixture.content.string()})==0,
            "converter CLI accepts a valid tree with own and base PFX names");
    resources.zenCache.clear();
    const auto* tree=loadVobBundle("firetree_lamp.ZeN");
    require(tree && tree->size()==4 && (*tree)[0]->visual_name=="OWN_TEST_SMOKE.PFX" &&
            (*tree)[1]->visual_name=="FIRE_MEDIUM.PFX" && (*tree)[2]->visual_name=="VOB_LAYER_TEST_CUBE.3DS",
            "a case-insensitive bundle name loads the complete own tree with canonical resource names");
    const auto& flame=*(*tree)[0];
    require(flame.position.x==100 && flame.position.y==200 && flame.position.z==300 &&
            std::abs(flame.rotation[0][0])<.0001f && std::abs(std::abs(flame.rotation[0][2])-1.f)<.0001f,
            "tree emitter preserves its slot-local centimetres and yaw");
    const auto* light=dynamic_cast<const asset::VLight*>((*tree)[3].get());
    require(light && light->range==450 && light->color.r==255 && light->color.g==128 && light->color.b==0 &&
            light->position.x==1 && light->position.y==2 && light->position.z==3 &&
            light->rotation[2][0]==1 && light->rotation[0][2]==-1,
            "tree light preserves its range, color and slot-local position and rotation");
    require(light->color_animation_list.size()==2 && light->color_animation_list[1].r==200 &&
            light->color_animation_list[1].g==90 && light->color_animation_list[1].b==10 &&
            light->color_animation_fps==12 && light->range_animation_scale.size()==2 &&
            light->range_animation_scale[1]==0.85f && light->range_animation_fps==12,
            "tree light preserves color and range animation keys and fps");
    {
      VobBundle bundle(world,"FIRETREE_LAMP.ZEN",Vob::None);
      require(bundle.rootVobs.size()==4,"own tree creates every runtime root");
      bundle.setObjMatrix(Tempest::Matrix4x4(0,0,1,1000, 0,1,0,2000, -1,0,0,3000, 0,0,0,1));
      require(bundle.rootVobs[0]->position()==Tempest::Vec3(1300,2200,2900) &&
              bundle.rootVobs[3]->position()==Tempest::Vec3(1003,2002,2999) &&
              std::abs(bundle.rootVobs[0]->transform().at(0,0)+1)<.0001f &&
              std::abs(bundle.rootVobs[3]->transform().at(2,2)+1)<.0001f,
              "runtime slot rotation and translation compose with authored root transforms");
      bundle.setObjMatrix(Tempest::Matrix4x4(1,0,0,-10, 0,1,0,-20, 0,0,1,-30, 0,0,0,1));
      require(bundle.rootVobs[0]->position()==Tempest::Vec3(90,180,270) &&
              bundle.rootVobs[3]->position()==Tempest::Vec3(-9,-18,-27) &&
              std::abs(bundle.rootVobs[0]->transform().at(2,0)-1)<.0001f &&
              bundle.rootVobs[3]->transform().at(0,2)==-1,
              "updating the slot reuses authored transforms without accumulating the previous slot");
      }
    fs::remove(file);
    require(loadVobBundle("firetree_lamp.ZeN")==tree,"own bundle is cached after its file disappears");
    textFile(file,valid);
    for(const char* flag:{"OPENGOTHIC_VOB_LAYERS","OPENGOTHIC_CONTENT"}) {
      setenv(flag,std::string_view(flag)=="OPENGOTHIC_CONTENT" ? "" : "0",1);
      resources.zenCache.clear();
      fallback(flag);
      if(std::string_view(flag)=="OPENGOTHIC_CONTENT") setenv(flag,fixture.content.c_str(),1);
      else unsetenv(flag);
      }
    const std::vector<std::pair<std::string,std::string>> bad={
      {"kind",R"({"format":1,"add":[{"key":"unknown","kind":"npc"}]})"},
      {"remove",R"({"format":1,"add":[],"remove":[]})"},
      {"NO_SUCH_PFX","{\"format\":1,\"add\":["+emitter("valid-first","FIRE_SMOKE")+","+
                        emitter("unknown","NO_SUCH_PFX")+"]}"},
      {"format",R"({"format":2,"add":[]})"}
      };
    for(const auto& [reason,json]:bad) {
      textFile(file,json);
      resources.zenCache.clear();
      checkedTree.clear();
      const auto rejected=VobLayer::readTree(checkedTree,baseRoot,file,fixture.content);
      require(!rejected.errors.empty() && checkedTree.empty() &&
              rejected.errors.front().find(file.string())!=std::string::npos &&
              rejected.errors.front().find(reason)!=std::string::npos,
              reason+": offline tree validation rejects the complete file with its name and reason");
      size_t errors=0;
      std::string logged;
      Tempest::Log::setOutputCallback([&](Tempest::Log::Mode mode,const char* line) {
        if(mode==Tempest::Log::Error) { ++errors; logged=line; }
        });
      const auto* rejectedTree=fallback(reason);
      require(loadVobBundle("FIRETREE_LAMP.ZEN")==rejectedTree,reason+": rejected tree fallback is cached");
      Tempest::Log::setOutputCallback({});
      require(errors==1 && logged.find(file.string())!=std::string::npos && logged.find(reason)!=std::string::npos,
              reason+": one runtime error names the rejected tree file and reason");
      require(subprocess({GOTHIC_CONTENT_CONVERT,"check-vob-tree",baseRoot.string(),file.string(),fixture.content.string()})!=0,
              reason+": converter CLI rejects an invalid tree");
      }
    fs::remove(file);
    resources.zenCache.clear();
    }
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
      loadWorld(data,worldMesh,"TEST.ZEN");
      Tempest::Log::setOutputCallback({});
      require(errors==1 && WorldData::compare(Fixture::original(),data).empty(),
              "running Resources rejects a late mesh entry atomically using its startup registry");
      }
    {
      Resources resources;
      resources.automaticBase.path=fixture.base;
      WorldData::Data data;
      asset::WorldMesh worldMesh;
      loadWorld(data,worldMesh,"TEST.ZEN");
      require(data.vobs.size()==4 && data.vobs.back()->vob_name=="late-mesh" && VobLayer::isAdded(*data.vobs.back()),
              "fresh Resources accepts the mesh entry and applies the layer");
      resources.baseFingerprint="particle-height-contract";
      const auto originalFingerprint=Resources::wayHeightInputs("TEST.ZEN").contentFingerprint;
      require(!originalFingerprint.empty(),"height fingerprint is available for the fixture content");
      const auto particles=fixture.content/"particles.d/height-input.json";
      textFile(particles,R"({"format":1,"effects":[]})");
      const auto addedFingerprint=Resources::wayHeightInputs("TEST.ZEN").contentFingerprint;
      require(!addedFingerprint.empty() && addedFingerprint!=originalFingerprint,
              "adding a particle file invalidates the height fingerprint of an atomic mixed layer");
      textFile(particles,R"({"format":1,"effects":[]} )");
      const auto changedFingerprint=Resources::wayHeightInputs("TEST.ZEN").contentFingerprint;
      require(!changedFingerprint.empty() && changedFingerprint!=addedFingerprint,
              "editing a particle file invalidates the height fingerprint");
      fs::remove(particles);
      require(Resources::wayHeightInputs("TEST.ZEN").contentFingerprint==originalFingerprint,
              "removing the particle file restores the original height fingerprint");
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
    const char* args[]={"gothic-vob-layer-test","-g",gameDir};
    CommandLine commandLine(3,args);
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
      if(phase==3) setenv("OPENGOTHIC_CONTENT","",1);
      Resources resources;
      Gothic gothic;
      if(phase==0) particleDefinitions(fixture,Resources::baseDir());
      if(phase==0) flameLayer(fixture,Resources::baseDir());
      gothic.isNpcInDialogFn=[](const Npc*) { return false; };
      gothic.setupGlobalScripts();
      if(phase==0) runtimeParticles(gothic,fixture,Resources::baseDir());
      if(phase==2)
        require(gothic.loadParticleFx("OWN_TEST_SMOKE")!=nullptr,"disabling vob layers retains custom particle definitions");
      if(phase==3)
        require(gothic.loadParticleFx("OWN_TEST_SMOKE",true)==nullptr,"empty content exposes no custom particle names");
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
        vobTrees(fixture,resources,world);
        baseRoots=world.wobj.rootVobs.size();
        baseline=save(world);
        require(!baseline.empty(),"save without layer writes an archive");
        textFile(layer,"{\"format\":1,\"remove\":["+std::to_string(removed)+"],\"move\":[{\"id\":"+
                 std::to_string(moved)+",\"position\":[111,222,333],\"yaw_deg\":0}],\"add\":["+
                 mesh("save-cube","VOB_LAYER_TEST_CUBE","mesh")+","+
                 emitter("save-smoke","own_test_smoke.pfx")+","+
                 emitter("save-fire","fire_medium")+","+
                 emitter("save-sparks","Fire_Sparks.PfX")+
                 R"(,{"key":"save-flies","kind":"emitter","pfx":"FLIES","position":[10,20,30],"rotation":[[0,0,1],[0,1,0],[-1,0,0]]},{"key":"save-light","kind":"light","position":[10,20,30],"yaw_deg":0,"range":400,"color":[255,100,20]}]})");
        const auto baseWorld=Resources::baseDir()/"worlds/NEWWORLD";
        const auto checked=VobLayer::check(baseWorld,layer,fixture.content);
        require(checked.errors.empty() && checked.added==6,"offline checker accepts base and custom PFX names in different cases with optional suffix");
        require(subprocess({GOTHIC_CONTENT_CONVERT,"check-vob-layer",baseWorld.string(),layer.string(),fixture.content.string()})==0,
                "converter CLI resolves emitters from base programs and particles.d");
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
                  (vob->visual_name=="OWN_TEST_SMOKE.PFX" || vob->visual_name=="FIRE_MEDIUM.PFX" ||
                   vob->visual_name=="FIRE_SPARKS.PFX" || vob->visual_name=="FLIES.PFX") && vob->visual->name==vob->visual_name,
                  "layer emitter is a visible, noncolliding native PFX zCVob with a canonical name");
          }
        require(emitters==4,"all four base and custom particle effects use the native emitter visual path");
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
        require(gothic.loadParticleFx("OWN_TEST_SMOKE")==gothic.loadParticleFx("own_test_smoke.pfx"),
                "custom definitions are shared once across worlds");
        gothic.clearGame().reset();
        gothic.setGame(std::make_unique<GameSession>("NEWWORLD.ZEN"));
        require(layerCount(*gothic.world())==6,"returning to the layered world creates its additions exactly once");
        require(gothic.loadParticleFx("OWN_TEST_SMOKE")->lspPartAvg==12000,
                "returning to the original world retains its custom definition");
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
    setenv("OPENGOTHIC_CONTENT",fixture.content.c_str(),1);
    }
  };

int main() {
  try {
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
    particleMotion();
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

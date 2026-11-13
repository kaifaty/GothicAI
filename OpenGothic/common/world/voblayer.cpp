#include "voblayer.h"
#include "movablevobs.h"
#include "gothicglb.h"
#include "game/definitions/particlecontent.h"

#include <Tempest/Log>
#include <rapidjson/document.h>
#include <rapidjson/error/en.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_map>

namespace VobLayer {
namespace fs=std::filesystem;
using namespace asset;
using Json=rapidjson::Value;
using gothicglb::jsonString;
namespace {
using Vob = std::shared_ptr<asset::VirtualObject>;
struct LayerVob final : asset::VirtualObject, Added {};
struct Context {
  std::vector<Vob> removed;
  const std::map<std::string,fs::path>* ownMeshes = nullptr;
  std::unique_ptr<daedalus::DaedalusVm> particles;
  ParticleContent::Result ownParticles;
  };

[[noreturn]] void fail(const std::string& reason) { throw std::runtime_error(reason); }
void require(bool condition,const std::string& reason) { if(!condition) fail(reason); }
const Json& member(const Json& obj,const char* name) {
  require(obj.IsObject() && obj.HasMember(name),std::string("missing field ")+name);
  return obj[name];
  }
void array(const Json& obj) { require(obj.IsArray(),"expected array"); }
void array(const Json& obj,size_t size) {
  array(obj); require(obj.Size()==size,"expected array of "+std::to_string(size)+" elements");
  }
rapidjson::Document document(const fs::path& file) {
  const auto bytes=gothicglb::readFile(file);
  rapidjson::Document doc;
  doc.Parse<rapidjson::kParseFullPrecisionFlag|rapidjson::kParseValidateEncodingFlag>(
      reinterpret_cast<const char*>(bytes.data()),bytes.size());
  require(!doc.HasParseError(),"JSON error at "+std::to_string(doc.GetErrorOffset())+": "+
          rapidjson::GetParseError_En(doc.GetParseError()));
  require(doc.IsObject(),"expected object");
  const auto& format=member(doc,"format");
  require(format.IsInt() && format.GetInt()==1,"unsupported format");
  return doc;
  }

void visitVobs(const std::vector<Vob>& tree, const std::function<void(const Vob&)>& fn) {
  for(const auto& v:tree) {
    fn(v);
    visitVobs(v->children,fn);
    }
  }
std::string meshKey(std::string name) {
  require(!name.empty() && name.find_first_of("/\\") == std::string::npos &&
          name.find('\0') == std::string::npos,"invalid visual name");
  for(auto& c:name) c=char(std::toupper(uint8_t(c)));
  const auto extension=fs::path(name).extension().string();
  require(extension.empty() || extension==".3DS" || extension==".MRM","expected static mesh visual");
  return extension.empty() ? name : name.substr(0,name.size()-extension.size());
  }
std::string particleName(std::string name, Context& context, const fs::path& baseRoot,
                         const fs::path& contentRoot) {
  require(!name.empty() && name.find_first_of("/\\")==std::string::npos &&
          name.find('\0')==std::string::npos,"invalid PFX name");
  for(auto& c:name) c=char(std::toupper(uint8_t(c)));
  const auto extension=fs::path(name).extension().string();
  require(extension.empty() || extension==".PFX","expected PFX name");
  if(!extension.empty()) name.resize(name.size()-extension.size());
  if(context.particles==nullptr) {
    context.particles=ParticleContent::openProgram(baseRoot);
    context.ownParticles=ParticleContent::load(*context.particles,contentRoot);
    }
  require(context.ownParticles.effects.count(name)!=0 || ParticleContent::hasBase(*context.particles,name),
          "unknown PFX "+name);
  return name+".PFX";
  }
void layerFields(const Json& obj, std::initializer_list<const char*> allowed) {
  require(obj.IsObject(),"expected object");
  std::set<std::string> names;
  for(const auto& field:obj.GetObject()) {
    const std::string name(field.name.GetString(),field.name.GetStringLength());
    require(names.insert(name).second,"duplicate field "+name);
    require(std::any_of(allowed.begin(),allowed.end(),[&](const char* item) { return name==item; }),
            "unknown field "+name);
    }
  }
std::string layerString(const Json& value) {
  require(value.IsString(),"expected string");
  std::string result(value.GetString(),value.GetStringLength());
  require(!result.empty() && result.find('\0')==std::string::npos,"expected nonempty string without NUL");
  return result;
  }
float layerFloat(const Json& value) {
  require(value.IsNumber(),"expected number");
  const auto number=value.GetDouble();
  require(std::isfinite(number) && std::abs(number)<=std::numeric_limits<float>::max(),"number out of range");
  return float(number);
  }
Vec3 layerPosition(const Json& obj) {
  const auto& value=member(obj,"position"); array(value,3);
  return {layerFloat(value[0]),layerFloat(value[1]),layerFloat(value[2])};
  }
Mat3 layerRotation(const Json& obj) {
  require(obj.HasMember("rotation")!=obj.HasMember("yaw_deg"),"expected rotation or yaw_deg, exclusively");
  auto rotation=Mat3::identity();
  if(obj.HasMember("yaw_deg")) {
    const double yaw=double(layerFloat(obj["yaw_deg"]))*3.14159265358979323846/180.;
    rotation[0][0]=float(std::cos(yaw)); rotation[2][0]=float(std::sin(yaw));
    rotation[0][2]=-rotation[2][0]; rotation[2][2]=rotation[0][0];
    } else {
    const auto& value=obj["rotation"]; array(value,3);
    for(unsigned r=0;r<3;++r) {
      array(value[r],3);
      for(unsigned c=0;c<3;++c) rotation[c][r]=layerFloat(value[r][c]);
      }
    for(unsigned a=0;a<3;++a) for(unsigned b=0;b<3;++b) {
      double dot=0;
      for(unsigned r=0;r<3;++r) dot+=double(rotation[a][r])*rotation[b][r];
      require(std::abs(dot-(a==b ? 1. : 0.))<0.001,"rotation must be orthonormal (no scale)");
      }
    const auto& a=rotation[0]; const auto& b=rotation[1]; const auto& c=rotation[2];
    const double determinant=double(a.x)*(double(b.y)*c.z-double(b.z)*c.y)-
                             double(b.x)*(double(a.y)*c.z-double(a.z)*c.y)+
                             double(c.x)*(double(a.y)*b.z-double(a.z)*b.y);
    require(std::abs(determinant-1.)<0.001,"rotation must have determinant 1");
    }
  return rotation;
  }
asset::Color layerColor(const Json& value, const char* name) {
  array(value,3);
  for(unsigned i=0;i<3;++i)
    require(value[i].IsUint() && value[i].GetUint()<=255,std::string(name)+" must contain integers from 0 to 255");
  return {uint8_t(value[0].GetUint()),uint8_t(value[1].GetUint()),uint8_t(value[2].GetUint()),255};
  }
// the fields a lamp added by the layer and a base lamp edited in place share; animated: the entry gives it colour or
// range keys of its own
void layerLamp(const Json& entry, LampExtra& lamp, bool animated) {
  // the flame's size (cm): the soft edge of the light's shadow, 6 by default (a torch)
  if(entry.HasMember("source_cm")) {
    lamp.sourceCm=layerFloat(entry["source_cm"]);
    require(lamp.sourceCm>0 && lamp.sourceCm<=500,"source_cm must be in 0..500");
    }
  // the lamp's own limit of the light near it, in its light at the range, in place of the common one (light.frag):
  // a torch on a plastered wall
  if(entry.HasMember("near")) {
    lamp.nearLimit=layerFloat(entry["near"]);
    require(lamp.nearLimit>0 && lamp.nearLimit<=1000,"near must be in 0..1000");
    }
  // the brightness follows an emitter's flame signal, the position trembles by jitter_cm
  if(entry.HasMember("flicker_from")) {
    require(!animated,"flicker_from replaces color_ani and range_ani");
    lamp.flickerFrom=layerString(entry["flicker_from"]);
    if(entry.HasMember("jitter_cm")) {
      lamp.jitterCm=layerFloat(entry["jitter_cm"]);
      require(lamp.jitterCm>=0 && lamp.jitterCm<=10,"jitter_cm must be in 0..10");
      }
    } else require(!entry.HasMember("jitter_cm"),"jitter_cm without flicker_from");
  }
rapidjson::Document layerIndex(const fs::path& file) {
  const auto bytes=gothicglb::readFile(file);
  rapidjson::Document doc;
  doc.Parse<rapidjson::kParseValidateEncodingFlag>(reinterpret_cast<const char*>(bytes.data()),bytes.size());
  require(!doc.HasParseError() && doc.IsObject(),"invalid mesh index "+file.string());
  return doc;
  }
std::map<std::string,std::shared_ptr<Visual>> layerVisuals(const WorldData::Data& data, const Context& context,
                                                        const fs::path& base, const fs::path& contentRoot) {
  std::map<std::string,std::shared_ptr<Visual>> result;
  const auto staticVisual=[&](const std::string& key) {
    auto visual=std::make_shared<VisualMesh>();
    visual->type=VisualType::MESH; visual->name=key+".3DS";
    for(const auto& alias:{key,key+".3DS",key+".MRM"}) result[alias]=visual;
    };
  const auto baseVisual=[&](const Vob& v) {
    if(isAdded(*v) || !v->visual) return;
    switch(v->visual->type) {
      case VisualType::MESH:
      case VisualType::MULTI_RESOLUTION_MESH:
        try { staticVisual(meshKey(v->visual->name)); } catch(const std::exception&) {}
        break;
      case VisualType::MODEL:
      case VisualType::MORPH_MESH: {
        auto name=v->visual->name;
        for(auto& c:name) c=char(std::toupper(uint8_t(c)));
        result[name]=v->visual;
        break;
        }
      default: break;
      }
    };
  visitVobs(data.vobs,baseVisual);
  visitVobs(context.removed,baseVisual);
  if(fs::exists(base/"meshes/meshes.json")) {
    auto index=layerIndex(base/"meshes/meshes.json");
    if(index.HasMember("meshes") && index["meshes"].IsObject())
      for(const auto& entry:index["meshes"].GetObject())
        if(entry.value.IsString() && fs::is_regular_file(base/"meshes"/entry.value.GetString()))
          staticVisual(meshKey(entry.name.GetString()));
    }
  if(fs::exists(base/"models/models.json")) {
    auto index=layerIndex(base/"models/models.json");
    if(index.HasMember("files") && index["files"].IsObject())
      for(const auto& entry:index["files"].GetObject()) {
        if(!entry.value.IsString() || !fs::is_regular_file(base/"models"/entry.value.GetString())) continue;
        std::string name(entry.name.GetString(),entry.name.GetStringLength());
        for(auto& c:name) c=char(std::toupper(uint8_t(c)));
        const auto ext=fs::path(name).extension().string();
        std::shared_ptr<Visual> visual;
        if(ext==".MDL" || ext==".MDM" || ext==".MDS" || ext==".MSB") {
          visual=std::make_shared<VisualModel>(); visual->type=VisualType::MODEL;
          if(ext==".MSB") name=fs::path(name).replace_extension(".MDS").string();
          } else if(ext==".MMB") {
          visual=std::make_shared<VisualMorphMesh>(); visual->type=VisualType::MORPH_MESH;
          } else continue;
        visual->name=name; result[name]=visual;
        if(ext==".MDL") result[fs::path(name).replace_extension(".ASC").string()]=visual;
        if(ext==".MMB") result[fs::path(name).replace_extension(".MMS").string()]=visual;
        }
    }
  if(context.ownMeshes!=nullptr) {
    for(const auto& [name,file]:*context.ownMeshes)
      if(fs::is_regular_file(file)) staticVisual(name);
    return result;
    }
  if(!contentRoot.empty() && fs::exists(contentRoot/"meshes/meshes.json")) {
    const auto root=contentRoot/"meshes";
    rapidjson::Document index;
    try { index=layerIndex(root/"meshes.json"); }
    catch(const std::exception&) { return result; }
    for(const auto& entry:index.GetObject()) {
      try {
        const std::string key(entry.name.GetString(),entry.name.GetStringLength());
        require(fs::path(key).extension().empty(),"mesh key must not have an extension");
        const auto name=meshKey(key);
        const fs::path path=layerString(member(entry.value,"glb"));
        auto extension=path.extension().string();
        for(auto& c:extension) c=char(std::toupper(uint8_t(c)));
        require(!path.is_absolute() && !path.has_root_name() && extension==".GLB","invalid GLB path");
        for(const auto& part:path) require(part!="..","invalid GLB path");
        require(fs::is_regular_file(root/path),"missing GLB");
        staticVisual(name);
        }
      catch(const std::exception&) { /* Invalid entries are unavailable visuals, as in Resources. */ }
      }
    }
  return result;
  }
std::vector<fs::path> layerFiles(const fs::path& worldDir) {
  std::vector<fs::path> result;
  if(!worldDir.empty() && fs::is_directory(worldDir/"vobs.d"))
    for(const auto& entry:fs::directory_iterator(worldDir/"vobs.d"))
      if(entry.is_regular_file() && entry.path().extension()==".json") result.push_back(entry.path());
  std::sort(result.begin(),result.end());
  return result;
  }
bool replaceLayerVob(std::vector<Vob>& tree, uint32_t id, const Vob& with) {
  for(auto& v:tree) {
    if(v->id==id && !isAdded(*v)) { v=with; return true; }
    if(replaceLayerVob(v->children,id,with)) return true;
    }
  return false;
  }
bool removeLayerVob(std::vector<Vob>& tree, uint32_t id, std::vector<Vob>& removed) {
  for(size_t i=0;i<tree.size();++i) {
    if(tree[i]->id==id && !isAdded(*tree[i])) {
      auto v=tree[i];
      tree.erase(tree.begin()+std::ptrdiff_t(i));
      tree.insert(tree.begin()+std::ptrdiff_t(i),v->children.begin(),v->children.end());
      removed.push_back(std::move(v));
      return true;
      }
    if(removeLayerVob(tree[i]->children,id,removed)) return true;
    }
  return false;
  }

const char* className(asset::VirtualObjectType type) {
  switch(type) {
    case asset::VirtualObjectType::UNKNOWN: return "UNKNOWN";
    case asset::VirtualObjectType::zCVob: return "zCVob";
    case asset::VirtualObjectType::zCVobLevelCompo: return "zCVobLevelCompo";
    case asset::VirtualObjectType::oCItem: return "oCItem";
    case asset::VirtualObjectType::oCNpc: return "oCNpc";
    case asset::VirtualObjectType::zCMoverController: return "zCMoverController";
    case asset::VirtualObjectType::zCVobScreenFX: return "zCVobScreenFX";
    case asset::VirtualObjectType::zCVobStair: return "zCVobStair";
    case asset::VirtualObjectType::zCPFXController: return "zCPFXController";
    case asset::VirtualObjectType::zCVobAnimate: return "zCVobAnimate";
    case asset::VirtualObjectType::zCVobLensFlare: return "zCVobLensFlare";
    case asset::VirtualObjectType::zCVobLight: return "zCVobLight";
    case asset::VirtualObjectType::zCVobSpot: return "zCVobSpot";
    case asset::VirtualObjectType::zCVobStartpoint: return "zCVobStartpoint";
    case asset::VirtualObjectType::zCMessageFilter: return "zCMessageFilter";
    case asset::VirtualObjectType::zCCodeMaster: return "zCCodeMaster";
    case asset::VirtualObjectType::zCTriggerWorldStart: return "zCTriggerWorldStart";
    case asset::VirtualObjectType::zCCSCamera: return "zCCSCamera";
    case asset::VirtualObjectType::zCCamTrj_KeyFrame: return "zCCamTrj_KeyFrame";
    case asset::VirtualObjectType::oCTouchDamage: return "oCTouchDamage";
    case asset::VirtualObjectType::zCTriggerUntouch: return "zCTriggerUntouch";
    case asset::VirtualObjectType::zCEarthquake: return "zCEarthquake";
    case asset::VirtualObjectType::oCMOB: return "oCMOB";
    case asset::VirtualObjectType::oCMobInter: return "oCMobInter";
    case asset::VirtualObjectType::oCMobBed: return "oCMobBed";
    case asset::VirtualObjectType::oCMobFire: return "oCMobFire";
    case asset::VirtualObjectType::oCMobLadder: return "oCMobLadder";
    case asset::VirtualObjectType::oCMobSwitch: return "oCMobSwitch";
    case asset::VirtualObjectType::oCMobWheel: return "oCMobWheel";
    case asset::VirtualObjectType::oCMobContainer: return "oCMobContainer";
    case asset::VirtualObjectType::oCMobDoor: return "oCMobDoor";
    case asset::VirtualObjectType::zCTrigger: return "zCTrigger";
    case asset::VirtualObjectType::zCTriggerList: return "zCTriggerList";
    case asset::VirtualObjectType::oCTriggerScript: return "oCTriggerScript";
    case asset::VirtualObjectType::oCTriggerChangeLevel: return "oCTriggerChangeLevel";
    case asset::VirtualObjectType::oCCSTrigger: return "oCCSTrigger";
    case asset::VirtualObjectType::zCMover: return "zCMover";
    case asset::VirtualObjectType::zCVobSound: return "zCVobSound";
    case asset::VirtualObjectType::zCVobSoundDaytime: return "zCVobSoundDaytime";
    case asset::VirtualObjectType::oCZoneMusic: return "oCZoneMusic";
    case asset::VirtualObjectType::oCZoneMusicDefault: return "oCZoneMusicDefault";
    case asset::VirtualObjectType::zCZoneZFog: return "zCZoneZFog";
    case asset::VirtualObjectType::zCZoneZFogDefault: return "zCZoneZFogDefault";
    case asset::VirtualObjectType::zCZoneVobFarPlane: return "zCZoneVobFarPlane";
    case asset::VirtualObjectType::zCZoneVobFarPlaneDefault: return "zCZoneVobFarPlaneDefault";
    }
  return "UNKNOWN";
  }
const asset::VirtualObject* saveBound(const asset::VirtualObject& vob) {
  if(vob.type!=asset::VirtualObjectType::zCVob || MovableVobs::classify(vob)!=nullptr) return &vob;
  for(const auto& child:vob.children)
    if(const auto* bound=saveBound(*child)) return bound;
  return nullptr;
  }

Result applyFile(WorldData::Data& data, Context& context, const fs::path& baseRoot, const fs::path& file,
                 const fs::path& contentRoot, bool treeOnly = false) {
  Result result;
  auto error=[&](const std::string& context,const std::exception& e) {
    auto text=file.string()+": "+context+e.what();
    std::replace(text.begin(),text.end(),'\n',' '); std::replace(text.begin(),text.end(),'\r',' ');
    result.errors.push_back(std::move(text));
    };
  try {
    auto doc=document(file);
    if(treeOnly) {
      layerFields(doc,{"format","add"});
      array(member(doc,"add"));
      } else {
      layerFields(doc,{"format","add","remove","move","light"});
      }
    std::unordered_map<uint32_t,Vob> base;
    std::set<uint32_t> ids;
    std::set<std::string> keys;
    auto collect=[&](const Vob& v) {
      ids.insert(v->id);
      if(!isAdded(*v)) base.emplace(v->id,v); else keys.insert(key(*v));
      };
    visitVobs(data.vobs,collect);
    visitVobs(context.removed,[&](const Vob& v) { ids.insert(v->id); });
    const auto visuals=layerVisuals(data,context,baseRoot,contentRoot);
    std::vector<Vob> additions;
    std::vector<uint32_t> removals;
    struct Move { Vob target; Vec3 position; Mat3 rotation; };
    std::vector<Move> moves;
    if(doc.HasMember("add")) {
      array(doc["add"]);
      for(const auto& entry:doc["add"].GetArray()) {
        std::string entryContext="add: ";
        try {
          const auto key=layerString(member(entry,"key")); entryContext="key "+jsonString(key)+": ";
          require(keys.insert(key).second,"duplicate key");
          const auto kind=layerString(member(entry,"kind"));
          Vob v;
          if(kind=="mesh") {
            layerFields(entry,{"key","kind","position","rotation","yaw_deg","visual","collision"});
            auto name=layerString(member(entry,"visual"));
            require(name.find_first_of("/\\")==std::string::npos,"invalid visual name");
            for(auto& c:name) c=char(std::toupper(uint8_t(c)));
            require(visuals.count(name)!=0,"unknown visual "+name);
            const auto collision=layerString(member(entry,"collision"));
            require(collision=="none" || collision=="mesh","expected collision none or mesh");
            v=std::make_shared<LayerVob>();
            v->type=VirtualObjectType::zCVob; v->show_visual=true; v->cd_static=collision=="mesh";
            v->visual=visuals.at(name);
            v->visual_name=v->visual->name; v->associated_visual_type=v->visual->type;
            } else if(kind=="emitter") {
            layerFields(entry,{"key","kind","position","rotation","yaw_deg","pfx","flicker"});
            auto visual=std::make_shared<VisualParticleEffect>();
            visual->type=VisualType::PARTICLE_EFFECT;
            visual->name=particleName(layerString(member(entry,"pfx")),context,baseRoot,contentRoot);
            auto emitter=std::make_shared<Emitter>();
            // the flame's signal: smooth noise at pulse_hz plus trembling at jitter_hz, 1 +- amount (pfxmotion.h)
            if(entry.HasMember("flicker")) {
              const auto& f=entry["flicker"];
              layerFields(f,{"pulse_hz","jitter_hz","amount","seed"});
              auto& fl=emitter->flicker;
              fl.pulseHz=layerFloat(member(f,"pulse_hz"));
              require(fl.pulseHz>0 && fl.pulseHz<=30,"flicker.pulse_hz must be in 0..30");
              fl.jitterHz=layerFloat(member(f,"jitter_hz"));
              require(fl.jitterHz>=0 && fl.jitterHz<=60,"flicker.jitter_hz must be in 0..60");
              fl.amount=layerFloat(member(f,"amount"));
              require(fl.amount>0 && fl.amount<=1,"flicker.amount must be in 0..1");
              const auto& seed=member(f,"seed");
              require(seed.IsUint(),"flicker.seed must be an unsigned integer");
              fl.seed=seed.GetUint();
              }
            v=std::move(emitter);
            v->type=VirtualObjectType::zCVob; v->show_visual=true; v->cd_static=false;
            v->visual=std::move(visual);
            v->visual_name=v->visual->name; v->associated_visual_type=v->visual->type;
            } else if(kind=="light") {
            layerFields(entry,{"key","kind","position","rotation","yaw_deg","range","color","color_ani","range_ani","ani_fps","cookie","source_cm","near","flicker_from","jitter_cm"});
            auto light=std::make_shared<Light>();
            light->type=VirtualObjectType::zCVobLight; light->light_type=LightType::POINT;
            light->range=layerFloat(member(entry,"range")); require(light->range>0,"range must be positive");
            light->color=layerColor(member(entry,"color"),"color");
            // flicker as the original's lights have it: colour and range keys looped at ani_fps, blended smoothly
            if(entry.HasMember("color_ani") || entry.HasMember("range_ani")) {
              const float fps=layerFloat(member(entry,"ani_fps"));
              require(fps>0 && fps<=60,"ani_fps must be in 0..60");
              if(entry.HasMember("color_ani")) {
                const auto& keys=entry["color_ani"];
                require(keys.IsArray() && keys.Size()>=2 && keys.Size()<=256,"color_ani must be an array of 2..256 colors");
                for(const auto& key:keys.GetArray())
                  light->color_animation_list.push_back(layerColor(key,"color_ani"));
                light->color_animation_fps=fps;
                }
              if(entry.HasMember("range_ani")) {
                const auto& keys=entry["range_ani"];
                require(keys.IsArray() && keys.Size()>=2 && keys.Size()<=256,"range_ani must be an array of 2..256 scales");
                for(const auto& key:keys.GetArray()) {
                  const float scale=layerFloat(key);
                  require(scale>0 && scale<=4,"range_ani scales must be in 0..4");
                  light->range_animation_scale.push_back(scale);
                  }
                light->range_animation_fps=fps;
                }
              } else require(!entry.HasMember("ani_fps"),"ani_fps without color_ani or range_ani");
            // a light mask (materials.json texture key, LightGroup): a lantern's cage on the light
            if(entry.HasMember("cookie")) {
              auto cookie=layerString(entry["cookie"]);
              for(auto& c:cookie) c=char(std::toupper(uint8_t(c)));
              require(!cookie.empty() && cookie.find_first_of("/\\")==std::string::npos &&
                      cookie.size()>4 && cookie.substr(cookie.size()-4)==".TGA","cookie: expected a texture name NAME.TGA");
              light->cookie=std::move(cookie);
              }
            layerLamp(entry,*light,entry.HasMember("color_ani") || entry.HasMember("range_ani"));
            light->is_static=true; light->on=true; light->can_move=false; light->cd_static=false;
            v=std::move(light);
            } else fail("expected kind mesh, light or emitter");
          v->position=layerPosition(entry); v->rotation=layerRotation(entry);
          dynamic_cast<Added&>(*v).key=key; v->vob_name=key; v->vob_static=true; v->cd_dynamic=v->cd_static; v->physics_enabled=false;
          uint32_t hash=2166136261u;
          for(const char c:key) hash=(hash^uint8_t(c))*16777619u;
          v->id=0x80000000u|(hash&0x7ffffffeu);
          require(ids.insert(v->id).second,"layer ID collision");
          additions.push_back(std::move(v));
          }
        catch(const std::exception& e) { error(entryContext,e); }
        }
      }
    // A base lamp edited in place, by its id: the colour (its own colour keys go), the range, and what the layer
    // gives its own lamps: "near", "source_cm", "flicker_from" (the base's colour and range keys go), "jitter_cm".
    // The id, the place and the tree stay, so a save made before or after the edit fits the lamp as it did
    std::vector<std::pair<uint32_t,std::shared_ptr<BaseLight>>> relit;
    if(doc.HasMember("light")) {
      array(doc["light"]);
      for(const auto& entry:doc["light"].GetArray()) {
        std::string entryContext="light: ";
        try {
          const auto& value=member(entry,"id");
          if(value.IsInt64()) entryContext="id "+std::to_string(value.GetInt64())+": ";
          else if(value.IsUint64()) entryContext="id "+std::to_string(value.GetUint64())+": ";
          require(value.IsUint(),"expected unsigned base ID");
          const auto id=value.GetUint();
          require(base.count(id)!=0,"unknown base ID");
          const auto& target=*base.at(id);
          const auto* lamp=dynamic_cast<const asset::VLight*>(&target);
          require(lamp!=nullptr,"class "+std::string(className(target.type))+" is not a lamp");
          require(std::none_of(relit.begin(),relit.end(),[&](const auto& r) { return r.first==id; }),"duplicate light id");
          layerFields(entry,{"id","color","range","near","source_cm","flicker_from","jitter_cm"});
          require(entry.MemberCount()>1,"nothing to change");
          const auto* earlier=dynamic_cast<const BaseLight*>(lamp);
          auto edited=earlier!=nullptr ? std::make_shared<BaseLight>(*earlier) : std::make_shared<BaseLight>(*lamp);
          if(entry.HasMember("color")) {
            edited->color=layerColor(entry["color"],"color");
            edited->color_animation_list.clear();
            }
          if(entry.HasMember("range")) {
            edited->range=layerFloat(entry["range"]);
            require(edited->range>0,"range must be positive");
            }
          layerLamp(entry,*edited,false);
          if(entry.HasMember("flicker_from")) {
            edited->color_animation_list.clear();
            edited->range_animation_scale.clear();
            }
          relit.emplace_back(id,std::move(edited));
          }
        catch(const std::exception& e) { error(entryContext,e); }
        }
      }
    // a lamp's flicker_from names an emitter with "flicker" added by this or an earlier file
    const auto signal=[&](LampExtra& lamp, const std::string& label) {
      if(lamp.flickerFrom.empty()) return;
      const Emitter* source=nullptr;
      auto look=[&](const Vob& o) {
        if(auto* e=dynamic_cast<const Emitter*>(o.get()); e!=nullptr && e->key==lamp.flickerFrom) source=e;
        };
      for(const auto& o:additions) look(o);
      if(source==nullptr) visitVobs(data.vobs,look);
      if(source==nullptr || !source->flicker.active())
        result.errors.push_back(file.string()+": "+label+": flicker_from: no emitter "+
                                jsonString(lamp.flickerFrom)+" with flicker");
      else
        lamp.flicker=source->flicker;
      };
    for(auto& v:additions)
      if(auto* light=dynamic_cast<Light*>(v.get())) signal(*light,"key "+jsonString(light->key));
    for(auto& [id,lamp]:relit) signal(*lamp,"id "+std::to_string(id));
    for(const char* operation:{"remove","move"}) if(doc.HasMember(operation)) {
      array(doc[operation]);
      for(const auto& entry:doc[operation].GetArray()) {
        std::string context=std::string(operation)+": ";
        try {
          const bool moving=std::string_view(operation)=="move";
          const auto& value=moving ? member(entry,"id") : entry;
          if(value.IsInt64()) context="id "+std::to_string(value.GetInt64())+": ";
          else if(value.IsUint64()) context="id "+std::to_string(value.GetUint64())+": ";
          require(value.IsUint(),"expected unsigned base ID");
          const auto id=value.GetUint();
          require(base.count(id)!=0,"unknown base ID");
          const auto& target=*base.at(id);
          if(const auto* bound=saveBound(target)) {
            const auto kind=std::string(className(target.type));
            const auto affected="id "+std::to_string(bound->id)+" class "+className(bound->type);
            fail("class "+kind+" cannot "+operation+": "+affected+" has base-ID or tree-bound saved state");
            }
          if(moving) {
            layerFields(entry,{"id","position","rotation","yaw_deg"});
            moves.push_back({base.at(id),layerPosition(entry),layerRotation(entry)});
            } else {
            removals.push_back(id);
            base.erase(id);
            }
          }
        catch(const std::exception& e) { error(context,e); }
        }
      }
    if(!result.errors.empty()) return result;
    // before the removals: a lamp's copy holds the children the lamp had when the file was read
    for(const auto& [id,lamp]:relit)
      if(replaceLayerVob(data.vobs,id,lamp)) ++result.lit;
    for(const auto id:removals)
      if(removeLayerVob(data.vobs,id,context.removed)) ++result.removed;
    for(auto& move:moves) {
      move.target->position=move.position; move.target->rotation=move.rotation;
      ++result.moved;
      }
    result.added=additions.size();
    data.vobs.insert(data.vobs.end(),additions.begin(),additions.end());
    }
  catch(const std::exception& e) { error({},e); }
  return result;
  }
} // namespace

bool isAdded(const asset::VirtualObject& vob) {
  return dynamic_cast<const Added*>(&vob)!=nullptr;
  }

const std::string& key(const asset::VirtualObject& vob) {
  if(const auto* added=dynamic_cast<const Added*>(&vob)) return added->key;
  static const std::string empty;
  return empty;
  }

Result apply(WorldData::Data& data,const fs::path& baseWorldDir,const fs::path& worldContentDir,
             const std::map<std::string,fs::path>* ownMeshes) {
  Result total;
  if(const char* flag=std::getenv("OPENGOTHIC_VOB_LAYERS"); flag!=nullptr && std::string_view(flag)=="0")
    return total;
  std::vector<fs::path> files;
  try {
    files=layerFiles(worldContentDir);
    }
  catch(const fs::filesystem_error& error) {
    total.errors.push_back((worldContentDir/"vobs.d").string()+": "+error.what());
    Tempest::Log::e("vob layer: skipped ",total.errors.back());
    return total;
    }
  Context context;
  context.ownMeshes = ownMeshes;
  for(const auto& file:files) {
    const auto result=applyFile(data,context,baseWorldDir.parent_path().parent_path(),file,worldContentDir.parent_path().parent_path());
    if(result.errors.empty())
      Tempest::Log::i("vob layer: ",file.string(),": applied added=",result.added," removed=",result.removed," moved=",result.moved," lit=",result.lit);
    else
      for(const auto& error:result.errors) Tempest::Log::e("vob layer: skipped ",error);
    total.added+=result.added; total.removed+=result.removed; total.moved+=result.moved; total.lit+=result.lit;
    total.errors.insert(total.errors.end(),result.errors.begin(),result.errors.end());
    }
  total.errors.insert(total.errors.end(),context.ownParticles.errors.begin(),context.ownParticles.errors.end());
  return total;
  }

Result check(const fs::path& baseWorldDir,const fs::path& file,const fs::path& contentRoot) {
  const auto world=contentRoot.empty() ? fs::path{} : contentRoot/"worlds"/baseWorldDir.filename();
  auto data=WorldData::read(baseWorldDir,world);
  Context context;
  for(const auto& previous:layerFiles(world)) {
    if(previous.filename()>=file.filename()) break;
    applyFile(data,context,baseWorldDir.parent_path().parent_path(),previous,contentRoot);
    }
  auto result=applyFile(data,context,baseWorldDir.parent_path().parent_path(),file,contentRoot);
  result.errors.insert(result.errors.end(),context.ownParticles.errors.begin(),context.ownParticles.errors.end());
  return result;
  }

Result readTree(std::vector<Vob>& tree, const fs::path& baseRoot, const fs::path& file,
                const fs::path& contentRoot, const std::map<std::string,fs::path>* ownMeshes) {
  WorldData::Data data;
  Context context;
  context.ownMeshes=ownMeshes;
  auto result=applyFile(data,context,baseRoot,file,contentRoot,true);
  if(result.errors.empty()) tree=std::move(data.vobs);
  return result;
  }

} // namespace VobLayer

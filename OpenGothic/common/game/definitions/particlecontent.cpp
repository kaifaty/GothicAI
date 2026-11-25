#include "particlecontent.h"

#if defined(CONTENT_CONVERT_PARTICLE_PROGRAMS)
#include "definitiontypes.h"
#include "programdata.h"
#endif

#include "basestore.h"
#include "gothicglb.h"

#include <rapidjson/document.h>
#include <rapidjson/error/en.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>

namespace ParticleContent {
namespace fs=std::filesystem;
namespace {
using Json=rapidjson::Value;

std::string uppercase(std::string_view value) {
  std::string result(value);
  for(auto& c:result) c=char(std::toupper(uint8_t(c)));
  return result;
  }
void require(bool condition, const std::string& reason) {
  if(!condition) throw std::runtime_error(reason);
  }
std::string text(const Json& value) {
  require(value.IsString(),"expected string");
  return {value.GetString(),value.GetStringLength()};
  }
const Json& member(const Json& value, const char* name) {
  require(value.IsObject() && value.HasMember(name),std::string("missing field ")+name);
  return value[name];
  }
void fields(const Json& value, std::initializer_list<const char*> allowed) {
  require(value.IsObject(),"expected object");
  std::set<std::string> seen;
  for(const auto& field:value.GetObject()) {
    const auto name=text(field.name);
    require(seen.insert(name).second,"duplicate field "+name);
    require(std::any_of(allowed.begin(),allowed.end(),[&](const char* key) { return name==key; }),
            "unknown field "+name);
    }
  }
void overrideField(int32_t& field, const Json& value, const std::string& name) {
  require(value.IsInt(),name+": expected int");
  field=value.GetInt();
  }
void overrideField(float& field, const Json& value, const std::string& name) {
  require(value.IsNumber(),name+": expected float");
  const auto number=value.GetDouble();
  require(std::isfinite(number) && std::abs(number)<=std::numeric_limits<float>::max(),
          name+": float out of range");
  field=float(number);
  }
void overrideField(std::string& field, const Json& value, const std::string& name) {
  require(value.IsString(),name+": expected string");
  field=text(value);
  }
void overrideFields(const Catalog& catalog, Def::ParticleEffect& effect, const Json& values) {
  require(values.IsObject(),"set: expected object");
  std::set<std::string> seen;
  for(const auto& field:values.GetObject()) {
    const auto name=text(field.name);
    const auto key=uppercase(name);
    require(seen.insert(key).second,"duplicate field "+name);
    require(catalog.hasField(key),"unknown field "+name);
#define DEF_OVERRIDE_FIELD(type, fieldName, extent, symbolName) \
    if(key==symbolName) { \
      overrideField(effect.fieldName,field.value,name); \
      continue; \
      }
    DEF_PARTICLE_EFFECT_FIELDS(DEF_OVERRIDE_FIELD)
#undef DEF_OVERRIDE_FIELD
    throw std::runtime_error("unsupported field "+name);
    }
  }
float number(const Json& value, const char* name, double lo, double hi, const char* range) {
  require(value.IsNumber(),std::string(name)+": expected number");
  const auto v=value.GetDouble();
  require(std::isfinite(v) && v>=lo && v<=hi,std::string(name)+": expected "+range);
  return float(v);
  }
void readMotion(const Json& entry, Motion& motion) {
  if(entry.HasMember("curl")) {
    const auto& curl=entry["curl"];
    fields(curl,{"strength","frequency","speed"});
    motion.curlStrength =number(member(curl,"strength"),"curl.strength",0,20,"0..20");
    motion.curlFrequency=number(member(curl,"frequency"),"curl.frequency",0,50,"0..50");
    require(motion.curlFrequency>0,"curl.frequency: must be positive");
    motion.curlSpeed    =curl.HasMember("speed") ? number(curl["speed"],"curl.speed",0,10,"0..10") : 0.f;
    }
  if(entry.HasMember("drag"))      motion.drag     =number(entry["drag"],"drag",0,50,"0..50");
  if(entry.HasMember("stretch"))   motion.stretch  =number(entry["stretch"],"stretch",0,0.5,"0..0.5");
  if(entry.HasMember("minPixels")) motion.minPixels=number(entry["minPixels"],"minPixels",0,15,"0..15");
  if(entry.HasMember("hdr")) {
    const auto& hdr=entry["hdr"];
    fields(hdr,{"from","to","edge"});
    motion.hdrFrom=number(member(hdr,"from"),"hdr.from",0,50,"0..50");
    motion.hdrTo  =number(member(hdr,"to"),"hdr.to",0,200,"0..200");
    motion.hdrEdge=number(member(hdr,"edge"),"hdr.edge",0,1,"0..1");
    require(motion.hdrTo>motion.hdrFrom,"hdr.to: must be over hdr.from");
    }
  if(entry.HasMember("lean")) {
    const auto& lean=entry["lean"];
    require(lean.IsObject(),"lean: expected object");
    fields(lean,{"rise","max_deg","response","along","grow","grow_from"});
    motion.leanRise=number(member(lean,"rise"),"lean.rise",0.1,20,"0.1..20");
    if(lean.HasMember("max_deg"))  motion.leanMaxDeg  =number(lean["max_deg"],"lean.max_deg",0,88,"0..88");
    if(lean.HasMember("response")) motion.leanResponse=number(lean["response"],"lean.response",0.02,2,"0.02..2");
    if(lean.HasMember("along"))    motion.leanAlong   =number(lean["along"],"lean.along",0.05,1,"0.05..1");
    if(lean.HasMember("grow"))     motion.leanGrow    =number(lean["grow"],"lean.grow",0,2,"0..2");
    if(lean.HasMember("grow_from")) motion.leanGrowFrom=number(lean["grow_from"],"lean.grow_from",0.02,1,"0.02..1");
    }
  if(entry.HasMember("inherit")) motion.inherit=number(entry["inherit"],"inherit",0,1,"0..1");
  if(entry.HasMember("wind"))    motion.wind   =number(entry["wind"],"wind",0,2,"0..2");
  if(entry.HasMember("plume")) {
    const auto& plume=entry["plume"];
    require(plume.IsObject(),"plume: expected object");
    fields(plume,{"height","rise","wind"});
    motion.plumeHeight=number(member(plume,"height"),"plume.height",1,500,"1..500");
    motion.plumeRise  =number(member(plume,"rise"),"plume.rise",0.1,20,"0.1..20");
    motion.plumeWind  =number(member(plume,"wind"),"plume.wind",0,2,"0..2");
    }
  if(entry.HasMember("light")) {
    const auto& light=entry["light"];
    fields(light,{"range","color","keys","colors","fps","at","loop"});
    motion.lightRange=number(member(light,"range"),"light.range",50,2000,"50..2000");
    const auto triple=[](const Json& value, const char* name, double lo, double hi, const char* range, float* out) {
      require(value.IsArray() && value.Size()==3,std::string(name)+": expected three numbers");
      for(rapidjson::SizeType i=0; i<3; ++i)
        out[i]=number(value[i],name,lo,hi,range);
      };
    triple(member(light,"color"),"light.color",0,255,"0..255",motion.lightColor);
    motion.lightKeys.clear();
    motion.lightColors.clear();
    motion.lightFps=0;
    if(light.HasMember("keys")) {
      const auto& keys=light["keys"];
      require(keys.IsArray() && keys.Size()>=2 && keys.Size()<=64,"light.keys: expected 2..64 numbers");
      for(const auto& k:keys.GetArray())
        motion.lightKeys.push_back(number(k,"light.keys",0,2,"0..2"));
      }
    if(light.HasMember("colors")) {
      const auto& colors=light["colors"];
      require(colors.IsArray() && colors.Size()>=2 && colors.Size()<=64,"light.colors: expected 2..64 colours");
      for(const auto& c:colors.GetArray())
        triple(c,"light.colors",0,255,"0..255",motion.lightColors.emplace_back().data());
      }
    if(!motion.lightKeys.empty() || !motion.lightColors.empty())
      motion.lightFps=number(member(light,"fps"),"light.fps",0.1,60,"0.1..60");
    else require(!light.HasMember("fps"),"light.fps: needs light.keys or light.colors");
    for(auto& v:motion.lightAt) v=0;
    if(light.HasMember("at"))
      triple(light["at"],"light.at",-200,200,"-200..200",motion.lightAt);
    motion.lightLoop=false;
    if(light.HasMember("loop")) {
      require(light["loop"].IsBool(),"light.loop: expected boolean");
      motion.lightLoop=light["loop"].GetBool();
      }
    }
  if(entry.HasMember("haze")) {
    const auto& haze=entry["haze"];
    fields(haze,{"shift","cell"});
    motion.hazeShift=number(member(haze,"shift"),"haze.shift",0,10,"0..10");
    motion.hazeCell =number(member(haze,"cell"),"haze.cell",1,200,"1..200");
    }
  }
} // namespace

std::string canonicalName(std::string_view name) {
  auto result=uppercase(name);
  if(result.size()>=4 && result.compare(result.size()-4,4,".PFX")==0)
    result.resize(result.size()-4);
  return result;
  }

#if defined(CONTENT_CONVERT_PARTICLE_PROGRAMS)
namespace {
bool hasBase(const daedalus::DaedalusScript& script, std::string_view name) {
  const auto* symbol=script.find_symbol_by_name(canonicalName(name));
  const auto* cls=script.find_symbol_by_name("C_PARTICLEFX");
  if(symbol==nullptr || cls==nullptr || symbol->type()!=daedalus::DaedalusDataType::INSTANCE)
    return false;
  auto* parent=symbol;
  while(parent!=nullptr && parent!=cls) parent=script.find_symbol_by_index(parent->parent());
  return parent==cls;
  }

std::shared_ptr<Def::ParticleEffect> base(daedalus::DaedalusVm& vm, std::string_view name) {
  if(!hasBase(vm,name)) return nullptr;
  auto result=std::make_shared<VmDef::ParticleEffect>();
  result->vis_tex_is_quadpoly=1;
  vm.init_instance(result,canonicalName(name));
  return std::make_shared<Def::ParticleEffect>(result->copy());
  }
}

Catalog::Catalog(std::unique_ptr<daedalus::DaedalusVm> vm) : program(std::move(vm)) {
  }
#endif

Catalog::Catalog(EffectDefinitionData::Particles rows) {
  for(auto& row:rows)
    records.emplace(std::move(row.name),std::move(row.value));
  }

bool Catalog::contains(std::string_view name) const {
#if defined(CONTENT_CONVERT_PARTICLE_PROGRAMS)
  if(program!=nullptr)
    return hasBase(*program,name);
#endif
  return records.contains(canonicalName(name));
  }

bool Catalog::hasField(std::string_view name) const {
#if defined(CONTENT_CONVERT_PARTICLE_PROGRAMS)
  if(program!=nullptr) {
    const auto* cls=program->find_symbol_by_name("C_PARTICLEFX");
    const auto* symbol=program->find_symbol_by_name("C_PARTICLEFX."+std::string(name));
    return symbol!=nullptr && symbol->is_member() && cls!=nullptr && symbol->parent()==cls->index();
    }
#endif
#define PARTICLE_FIELD_NAME(type, field, extent, symbol) symbol,
  static constexpr std::string_view fields[]={DEF_PARTICLE_EFFECT_FIELDS(PARTICLE_FIELD_NAME)};
#undef PARTICLE_FIELD_NAME
  return std::find(std::begin(fields),std::end(fields),name)!=std::end(fields);
  }

std::shared_ptr<Def::ParticleEffect> Catalog::materialize(std::string_view name) const {
#if defined(CONTENT_CONVERT_PARTICLE_PROGRAMS)
  if(program!=nullptr)
    return base(*program,name);
#endif
  const auto it=records.find(canonicalName(name));
  if(it==records.end())
    return nullptr;
  return std::make_shared<Def::ParticleEffect>(it->second);
  }

std::optional<Catalog> openStockCatalog(const fs::path& base) {
  constexpr auto program=EffectDefinitionData::stockParticleProgram;
  const auto section=EffectDefinitionData::particleSection(base,program);
  try {
    if(fs::exists(section))
      return Catalog(EffectDefinitionData::readParticles(base,program));
    }
  catch(const std::runtime_error& error) {
    throw BaseStore::Error(section.string()+": "+error.what());
    }
  return std::nullopt;
  }

namespace {
Catalog openCatalog(const fs::path& baseRoot) {
#if defined(CONTENT_CONVERT_PARTICLE_PROGRAMS)
  const auto programs=baseRoot/"programs";
  if(fs::exists(programs/"programs.json")) {
    const auto index=ProgramData::readIndex(programs);
    const auto* source=ProgramData::particleFxProgram(index);
    require(source!=nullptr,"ParticleFx.dat missing from program base index");
    if(source->recordSha256==EffectDefinitionData::stockParticleProgram)
      if(auto stock=openStockCatalog(baseRoot))
        return std::move(*stock);
    std::string sha256;
    auto program=ProgramData::readProgram(index.path(*source),&sha256);
    require(sha256==source->recordSha256,"particle program identity differs from base index");
    auto script=daedalus::DaedalusScript::from_program(std::move(program));
    VmDef::ParticleEffect::register_(script);
    return Catalog(std::make_unique<daedalus::DaedalusVm>(std::move(script),
        daedalus::DaedalusVmExecutionFlag::ALLOW_NULL_INSTANCE_ACCESS));
    }
#endif
  if(auto stock=openStockCatalog(baseRoot))
    return std::move(*stock);
  throw BaseStore::Error("native game requires a prepared particle catalog");
  }
}

Result load(const Catalog& catalog, const fs::path& contentRoot) {
  Result result;
  if(contentRoot.empty()) return result;
  const auto directory=contentRoot/"particles.d";
  std::vector<fs::path> files;
  try {
    if(!fs::is_directory(directory)) return result;
    for(const auto& entry:fs::directory_iterator(directory))
      if(entry.is_regular_file() && entry.path().extension()==".json") files.push_back(entry.path());
    std::sort(files.begin(),files.end());
    }
  catch(const fs::filesystem_error& error) {
    result.errors.push_back(directory.string()+": "+error.what());
    return result;
    }
  for(const auto& file:files) {
    try {
      const auto bytes=gothicglb::readFile(file);
      rapidjson::Document doc;
      doc.Parse<rapidjson::kParseFullPrecisionFlag|rapidjson::kParseValidateEncodingFlag>(
          reinterpret_cast<const char*>(bytes.data()),bytes.size());
      require(!doc.HasParseError(),"JSON error at "+std::to_string(doc.GetErrorOffset())+": "+
              rapidjson::GetParseError_En(doc.GetParseError()));
      fields(doc,{"format","effects"});
      const auto& format=member(doc,"format");
      require(format.IsInt() && format.GetInt()==1,"unsupported format");
      const auto& effects=member(doc,"effects");
      require(effects.IsArray(),"effects: expected array");
      decltype(result.effects) pending;
      decltype(result.flipbooks) pendingFlipbooks;
      decltype(result.motions) pendingMotions;
      decltype(result.with) pendingWith;
      decltype(result.land) pendingLand;
      decltype(result.nodes) pendingNodes;
      for(const auto& entry:effects.GetArray()) {
        fields(entry,{"name","base","set","flipbook","curl","drag","stretch","minPixels","hdr","haze","lean","inherit","wind","plume","light","replaces","with","land","node"});
        const auto name=text(member(entry,"name"));
        require(!name.empty() && std::all_of(name.begin(),name.end(),[](char c) {
          return (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='_';
          }),"invalid effect name "+name+": expected [A-Z0-9_]+");
        // "replaces": the entry takes the place of the game's own effect of this name, wherever it is asked for
        bool replaces=false;
        if(entry.HasMember("replaces")) {
          require(entry["replaces"].IsBool(),"replaces: expected boolean");
          replaces=entry["replaces"].GetBool();
          }
        if(replaces) require(catalog.contains(name),"replaces: no base PFX "+name);
        else require(!catalog.contains(name),"effect name conflicts with base PFX "+name);
        require(result.effects.count(name)==0 && pending.count(name)==0,"duplicate effect "+name);
        const auto baseName=canonicalName(text(member(entry,"base")));
        std::shared_ptr<Def::ParticleEffect> original;
        if(const auto found=pending.find(baseName); found!=pending.end()) original=found->second;
        else if(const auto found=result.effects.find(baseName); found!=result.effects.end()) original=found->second;
        else original=catalog.materialize(baseName);
        require(original!=nullptr,"unknown base PFX "+baseName+" for "+name);
        auto definition=std::make_shared<Def::ParticleEffect>(*original);
        overrideFields(catalog,*definition,member(entry,"set"));
        // a flipbook comes with the entry, or from an own base that has one
        if(entry.HasMember("flipbook")) {
          const auto& fb=entry["flipbook"];
          fields(fb,{"cols","rows","frames","fps","yaw","steady","once"});
          auto integer=[&](const char* key) {
            const auto& v=member(fb,key);
            require(v.IsInt(),std::string("flipbook.")+key+": expected integer");
            return v.GetInt();
            };
          Flipbook book;
          book.cols=integer("cols"); book.rows=integer("rows"); book.frames=integer("frames");
          const auto& fps=member(fb,"fps");
          require(fps.IsNumber(),"flipbook.fps: expected number");
          book.fps=float(fps.GetDouble());
          require(book.cols>=1 && book.cols<=8 && book.rows>=1 && book.rows<=8,"flipbook: cols and rows are 1..8");
          require(book.frames>=1 && book.frames<=book.cols*book.rows,"flipbook: frames are 1..cols*rows");
          require(std::isfinite(book.fps) && book.fps>0 && book.fps<=120,"flipbook.fps: 0..120");
          if(fb.HasMember("yaw")) {
            require(fb["yaw"].IsBool(),"flipbook.yaw: expected boolean");
            book.yaw=fb["yaw"].GetBool();
            }
          if(fb.HasMember("steady")) {
            require(fb["steady"].IsBool(),"flipbook.steady: expected boolean");
            book.steady=fb["steady"].GetBool();
            }
          if(fb.HasMember("once")) {
            require(fb["once"].IsBool(),"flipbook.once: expected boolean");
            book.once=fb["once"].GetBool();
            }
          require(!(book.once && book.steady),"flipbook.once: not with steady");
          pendingFlipbooks.emplace(name,book);
          }
        else if(const auto f=pendingFlipbooks.find(baseName); f!=pendingFlipbooks.end())
          pendingFlipbooks.emplace(name,f->second);
        else if(const auto f=result.flipbooks.find(baseName); f!=result.flipbooks.end())
          pendingFlipbooks.emplace(name,f->second);
        // own motion and look: inherited field by field from an own base, then the entry's own
        Motion motion;
        if(const auto m=pendingMotions.find(baseName); m!=pendingMotions.end()) motion=m->second;
        else if(const auto m=result.motions.find(baseName); m!=result.motions.end()) motion=m->second;
        readMotion(entry,motion);
        if(motion.stretch>0) {
          const auto orientation=uppercase(definition->vis_orientation_s);
          require(orientation=="VELO" || orientation=="VELO3D","stretch: needs visOrientation_S VELO or VELO3D");
          }
        require(motion.minPixels==0 || pendingFlipbooks.count(name)==0,"minPixels: not with a flipbook");
        {
        const auto book=pendingFlipbooks.find(name);
        const bool steady=book!=pendingFlipbooks.end() && book->second.steady;
        require(motion.leanRise==0 || steady,"lean: needs a steady flipbook");
        require(motion.inherit==0 || !steady,"inherit: not with a steady flipbook");
        require(motion.stretch==0 || book==pendingFlipbooks.end(),"stretch: not with a flipbook");
        require(motion.wind==0 || motion.leanRise>0 || (motion.drag>0 && !steady),"wind: needs lean or drag");
        require(motion.plumeHeight==0 || (motion.wind>0 && !steady),"plume: needs wind, not with a steady flipbook");
        }
        require(motion.hdrTo==0 || uppercase(definition->vis_alpha_func_s)=="ADD","hdr: needs visAlphaFunc_S ADD");
        require(motion.hazeShift==0 || (motion.hdrTo==0 && pendingFlipbooks.count(name)==0),"haze: not with hdr or a flipbook");
        if(!motion.empty()) pendingMotions.emplace(name,motion);
        if(entry.HasMember("with")) {
          const auto& with=entry["with"];
          require(with.IsArray() && with.Size()>=1 && with.Size()<=8,"with: expected 1..8 effect names");
          std::vector<std::string> names;
          for(const auto& item:with.GetArray()) {
            const auto other=canonicalName(text(item));
            require(pending.count(other)!=0 || result.effects.count(other)!=0,"with: unknown own effect "+other);
            require(pendingWith.count(other)==0 && result.with.count(other)==0,"with: "+other+" brings effects of its own");
            require(std::find(names.begin(),names.end(),other)==names.end(),"with: duplicate "+other);
            names.push_back(other);
            }
          pendingWith.emplace(name,std::move(names));
          }
        if(entry.HasMember("land")) {
          // the particles fly in the world (not with their emitter), and the effect they land as lands no more
          const auto other=canonicalName(text(member(entry,"land")));
          require(pending.count(other)!=0 || result.effects.count(other)!=0,"land: unknown own effect "+other);
          require(pendingLand.count(other)==0 && result.land.count(other)==0,"land: "+other+" lands itself");
          require(definition->use_emitters_for==0,"land: needs useEmittersFOR 0");
          const auto book=pendingFlipbooks.find(name);
          require(book==pendingFlipbooks.end() || !book->second.steady,"land: not with a steady flipbook");
          pendingLand.emplace(name,other);
          }
        if(entry.HasMember("node")) {
          const auto& node=entry["node"];
          require(node.IsString(),"node: expected string");
          const auto nodeName=uppercase(text(node));
          require(!nodeName.empty() && nodeName.size()<=64,"node: expected 1..64 characters");
          pendingNodes.emplace(name,nodeName);
          }
        pending.emplace(name,std::move(definition));
        }
      result.effects.insert(pending.begin(),pending.end());
      result.flipbooks.insert(pendingFlipbooks.begin(),pendingFlipbooks.end());
      result.motions.insert(pendingMotions.begin(),pendingMotions.end());
      result.with.insert(pendingWith.begin(),pendingWith.end());
      result.land.insert(pendingLand.begin(),pendingLand.end());
      result.nodes.insert(pendingNodes.begin(),pendingNodes.end());
      }
    catch(const std::exception& error) { result.errors.push_back(file.string()+": "+error.what()); }
    }
  return result;
  }

#if defined(CONTENT_CONVERT_PARTICLE_PROGRAMS)
Result check(const fs::path& baseRoot, const fs::path& contentRoot) {
  return load(openCatalog(baseRoot),contentRoot);
  }
#endif

GameHost::ParticleNameCheck nameChecker() {
  struct State {
    std::optional<Catalog> catalog;
    Result own;
    };
  return [state=std::make_shared<State>()](std::string_view name, const fs::path& baseRoot,
                                         const fs::path& contentRoot, std::vector<std::string>& errors) {
    if(!state->catalog) {
      state->catalog.emplace(openCatalog(baseRoot));
      state->own=load(*state->catalog,contentRoot);
      }
    errors=state->own.errors;
    return state->own.effects.count(std::string(name))!=0 || state->catalog->contains(name);
    };
  }

} // namespace ParticleContent

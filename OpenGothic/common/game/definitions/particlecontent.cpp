#include "particlecontent.h"
#include "definitiontypes.h"

#include "gothicglb.h"
#include "programdata.h"

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
void overrideFields(daedalus::DaedalusVm& vm, Def::ParticleEffect& effect, const Json& values) {
  require(values.IsObject(),"set: expected object");
  const auto* cls=vm.find_symbol_by_name("C_PARTICLEFX");
  std::set<std::string> seen;
  for(const auto& field:values.GetObject()) {
    const auto name=text(field.name);
    const auto key=uppercase(name);
    require(seen.insert(key).second,"duplicate field "+name);
    auto* symbol=vm.find_symbol_by_name("C_PARTICLEFX."+key);
    require(symbol!=nullptr && symbol->is_member() && cls!=nullptr && symbol->parent()==cls->index(),
            "unknown field "+name);
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
  }
} // namespace

std::string canonicalName(std::string_view name) {
  auto result=uppercase(name);
  if(result.size()>=4 && result.compare(result.size()-4,4,".PFX")==0)
    result.resize(result.size()-4);
  return result;
  }

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

std::unique_ptr<daedalus::DaedalusVm> openProgram(const fs::path& baseRoot) {
  const auto index=ProgramData::readIndex(baseRoot/"programs");
  const auto* source=index.findVfs("ParticleFx.dat");
  if(source==nullptr) source=index.findHost("_work/Data/Scripts/_compiled/ParticleFx.dat");
  require(source!=nullptr,"ParticleFx.dat missing from program base index");
  std::string sha256;
  auto program=ProgramData::readProgram(index.path(*source),&sha256);
  require(sha256==source->recordSha256,"particle program identity differs from base index");
  auto script=daedalus::DaedalusScript::from_program(std::move(program));
  VmDef::ParticleEffect::register_(script);
  return std::make_unique<daedalus::DaedalusVm>(std::move(script),
      daedalus::DaedalusVmExecutionFlag::ALLOW_NULL_INSTANCE_ACCESS);
  }

Result load(daedalus::DaedalusVm& vm, const fs::path& contentRoot) {
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
      for(const auto& entry:effects.GetArray()) {
        fields(entry,{"name","base","set","flipbook","curl","drag","stretch","minPixels","hdr"});
        const auto name=text(member(entry,"name"));
        require(!name.empty() && std::all_of(name.begin(),name.end(),[](char c) {
          return (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='_';
          }),"invalid effect name "+name+": expected [A-Z0-9_]+");
        require(!hasBase(vm,name),"effect name conflicts with base PFX "+name);
        require(result.effects.count(name)==0 && pending.count(name)==0,"duplicate effect "+name);
        const auto baseName=canonicalName(text(member(entry,"base")));
        std::shared_ptr<Def::ParticleEffect> original;
        if(const auto found=pending.find(baseName); found!=pending.end()) original=found->second;
        else if(const auto found=result.effects.find(baseName); found!=result.effects.end()) original=found->second;
        else original=base(vm,baseName);
        require(original!=nullptr,"unknown base PFX "+baseName+" for "+name);
        auto definition=std::make_shared<Def::ParticleEffect>(*original);
        overrideFields(vm,*definition,member(entry,"set"));
        // a flipbook comes with the entry, or from an own base that has one
        if(entry.HasMember("flipbook")) {
          const auto& fb=entry["flipbook"];
          fields(fb,{"cols","rows","frames","fps","yaw","steady"});
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
        require(motion.hdrTo==0 || uppercase(definition->vis_alpha_func_s)=="ADD","hdr: needs visAlphaFunc_S ADD");
        if(!motion.empty()) pendingMotions.emplace(name,motion);
        pending.emplace(name,std::move(definition));
        }
      result.effects.insert(pending.begin(),pending.end());
      result.flipbooks.insert(pendingFlipbooks.begin(),pendingFlipbooks.end());
      result.motions.insert(pendingMotions.begin(),pendingMotions.end());
      }
    catch(const std::exception& error) { result.errors.push_back(file.string()+": "+error.what()); }
    }
  return result;
  }

Result check(const fs::path& baseRoot, const fs::path& contentRoot) {
  auto vm=openProgram(baseRoot);
  return load(*vm,contentRoot);
  }

} // namespace ParticleContent

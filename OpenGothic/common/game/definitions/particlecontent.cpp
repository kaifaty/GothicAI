#include "particlecontent.h"

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
void overrideFields(zenkit::DaedalusVm& vm, Def::ParticleEffect& effect, const Json& values) {
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
    const auto& value=field.value;
    switch(symbol->type()) {
      case zenkit::DaedalusDataType::INT:
        require(value.IsInt(),name+": expected int");
        symbol->set_int(value.GetInt(),0,&effect);
        break;
      case zenkit::DaedalusDataType::FLOAT: {
        require(value.IsNumber(),name+": expected float");
        const auto number=value.GetDouble();
        require(std::isfinite(number) && std::abs(number)<=std::numeric_limits<float>::max(),
                name+": float out of range");
        symbol->set_float(float(number),0,&effect);
        break;
        }
      case zenkit::DaedalusDataType::STRING:
        require(value.IsString(),name+": expected string");
        symbol->set_string(text(value),0,&effect);
        break;
      default: throw std::runtime_error("unsupported field "+name);
      }
    }
  }
} // namespace

std::string canonicalName(std::string_view name) {
  auto result=uppercase(name);
  if(result.size()>=4 && result.compare(result.size()-4,4,".PFX")==0)
    result.resize(result.size()-4);
  return result;
  }

bool hasBase(const zenkit::DaedalusScript& script, std::string_view name) {
  const auto* symbol=script.find_symbol_by_name(canonicalName(name));
  const auto* cls=script.find_symbol_by_name("C_PARTICLEFX");
  if(symbol==nullptr || cls==nullptr || symbol->type()!=zenkit::DaedalusDataType::INSTANCE)
    return false;
  auto* parent=symbol;
  while(parent!=nullptr && parent!=cls) parent=script.find_symbol_by_index(parent->parent());
  return parent==cls;
  }

std::shared_ptr<Def::ParticleEffect> base(zenkit::DaedalusVm& vm, std::string_view name) {
  if(!hasBase(vm,name)) return nullptr;
  auto result=std::make_shared<Def::ParticleEffect>();
  result->vis_tex_is_quadpoly=1;
  vm.init_instance(result,canonicalName(name));
  return result;
  }

std::unique_ptr<zenkit::DaedalusVm> openProgram(const fs::path& baseRoot) {
  const auto index=ProgramData::readIndex(baseRoot/"programs");
  const auto* source=index.findVfs("ParticleFx.dat");
  if(source==nullptr) source=index.findHost("_work/Data/Scripts/_compiled/ParticleFx.dat");
  require(source!=nullptr,"ParticleFx.dat missing from program base index");
  std::string sha256;
  auto program=ProgramData::readProgram(index.path(*source),&sha256);
  require(sha256==source->recordSha256,"particle program identity differs from base index");
  auto script=zenkit::DaedalusScript::from_program(std::move(program));
  Def::ParticleEffect::register_(script);
  return std::make_unique<zenkit::DaedalusVm>(std::move(script),
      zenkit::DaedalusVmExecutionFlag::ALLOW_NULL_INSTANCE_ACCESS);
  }

Result load(zenkit::DaedalusVm& vm, const fs::path& contentRoot) {
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
      for(const auto& entry:effects.GetArray()) {
        fields(entry,{"name","base","set"});
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
        pending.emplace(name,std::move(definition));
        }
      result.effects.insert(pending.begin(),pending.end());
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

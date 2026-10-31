#include "fracture.h"
#include "fracturedata.h"
#include "resources.h"

#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Tempest/Log>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <locale>
#include <mutex>
#include <sstream>
#include <unordered_set>

namespace {
std::string basename(std::string_view visual) {
  const auto slash=visual.find_last_of("/\\");
  if(slash!=std::string_view::npos) visual.remove_prefix(slash+1);
  const auto dot=visual.find_last_of('.');
  if(dot!=std::string_view::npos) visual=visual.substr(0,dot);
  std::string value(visual);
  std::transform(value.begin(),value.end(),value.begin(),[](unsigned char c) { return char(std::toupper(c)); });
  return value;
  }

bool matches(std::string_view pattern,std::string_view text) {
  size_t p=0,t=0,star=std::string_view::npos,retry=0;
  while(t<text.size()) {
    if(p<pattern.size() && (pattern[p]=='?' || pattern[p]==text[t])) { ++p; ++t; }
    else if(p<pattern.size() && pattern[p]=='*') { star=p++; retry=t; }
    else if(star!=std::string_view::npos) { p=star+1; t=++retry; }
    else return false;
    }
  while(p<pattern.size() && pattern[p]=='*') ++p;
  return p==pattern.size();
  }

bool finite(const FractureMesh::Vertex& v) {
  return std::isfinite(v.position.x) && std::isfinite(v.position.y) && std::isfinite(v.position.z) &&
         std::isfinite(v.normal.x) && std::isfinite(v.normal.y) && std::isfinite(v.normal.z) &&
         std::isfinite(v.uv.x) && std::isfinite(v.uv.y) && v.position.quadLength()<1.e12f;
  }
}

const FractureRule* FractureRule::classify(std::string_view visual) {
  static const auto rules=[] {
    std::vector<FractureRule> value;
    std::istringstream csv(destructibleVobsCsv);
    for(std::string line;std::getline(csv,line);) {
      if(line.empty() || line.front()=='#') continue;
      std::replace(line.begin(),line.end(),',',' ');
      std::istringstream row(line);
      row.imbue(std::locale::classic());
      FractureRule rule;
      if(row>>rule.visualPattern>>rule.className>>rule.health>>rule.threshold>>rule.impactDamage>>rule.arrowDamage>>
         rule.meleeWeapon>>rule.meleeStrength>>rule.fist>>rule.oneHand>>rule.twoHand)
        if(rule.health>0 && rule.threshold>0 && rule.impactDamage>0 && rule.arrowDamage>0 &&
           rule.meleeWeapon>0 && rule.meleeStrength>0 && rule.fist>0 && rule.oneHand>0 && rule.twoHand>0)
          value.push_back(std::move(rule));
      }
    return value;
    }();
  const auto name=basename(visual);
  for(const auto& rule:rules)
    if(matches(rule.visualPattern,name)) return &rule;
  return nullptr;
  }

float FractureRule::meleeDamage(Melee kind,float weaponDamage,float strength) const {
  if(!std::isfinite(weaponDamage) || !std::isfinite(strength)) return 0.f;
  const float power=std::max(weaponDamage,0.f)*meleeWeapon+std::max(strength,0.f)*meleeStrength;
  switch(kind) {
    case Melee::Fist:    return power*fist;
    case Melee::OneHand: return power*oneHand;
    case Melee::TwoHand: return power*twoHand;
    }
  return 0.f;
  }

std::shared_ptr<const FractureMesh> FractureMesh::load(std::string_view visual) {
  Resources::MeshReplacementSource source;
  const bool replacement=Resources::meshReplacementLoaded(visual,&source);
  std::filesystem::path path;
  if(replacement) {
    path=source.file;
    path.replace_extension(".fracture");
    }
  else {
    std::filesystem::path root;
    if(const auto* dir=std::getenv("OPENGOTHIC_FRACTURE")) {
      if(dir[0]=='\0') return {};
      root=dir;
      }
#ifdef _WIN32
    // the launcher's Path.home(); HOME is usually unset on Windows, the wide variable keeps a Cyrillic name
    else if(const auto* home=_wgetenv(L"USERPROFILE"); home && *home)
      root=std::filesystem::path(home)/".local/share/gothic-ai/fracture";
#else
    else if(const auto* home=std::getenv("HOME")) root=std::filesystem::path(home)/".local/share/gothic-ai/fracture";
#endif
    else return {};
    path=root/(basename(visual)+".fracture");
    }
  const auto read=[&]() -> std::shared_ptr<const FractureMesh> {
    std::ifstream file(path);
    file.imbue(std::locale::classic());
    std::string tag;
    size_t version=0,count=0;
    if(!(file>>tag>>version) || tag!="GOTHIC_FRACTURE" || version!=(replacement ? 2u : 1u)) return {};
    if(replacement) {
      size_t size=0;
      uint32_t crc=0;
      if(!(file>>tag>>size>>std::hex>>crc>>std::dec) || tag!="source" || size!=source.size || crc!=source.crc32) return {};
      }
    if(!(file>>tag>>count) || tag!="pieces" || count<8 || count>20) return {};
    auto mesh=std::make_shared<FractureMesh>();
    Physics::initialize();
    for(size_t p=0;p<count;++p) {
      Piece piece;
      size_t triangles=0;
      if(!(file>>tag>>piece.volume>>piece.center.x>>piece.center.y>>piece.center.z>>triangles) ||
         tag!="piece" || !std::isfinite(piece.volume) || piece.volume<=0 || triangles<4 || triangles>100000) return {};
      piece.triangles.resize(triangles);
      std::vector<JPH::Vec3> points;
      points.reserve(triangles*3);
      for(auto& triangle:piece.triangles) {
        if(!(file>>tag>>triangle.texture) || tag!="tri" || triangle.texture.size()>255) return {};
        for(auto& v:triangle.vertices) {
          if(!(file>>v.position.x>>v.position.y>>v.position.z>>v.normal.x>>v.normal.y>>v.normal.z>>v.uv.x>>v.uv.y) || !finite(v)) return {};
          points.emplace_back(v.position.x*.01f,v.position.y*.01f,v.position.z*.01f);
          }
        }
      JPH::ConvexHullShapeSettings settings(points.data(),int(points.size()),.001f);
      const auto shape=settings.Create();
      if(shape.HasError() || shape.Get()->GetVolume()<=0.f) return {};
      piece.hull=shape.Get();
      mesh->volume+=piece.volume;
      mesh->pieces.push_back(std::move(piece));
      }
    if(file>>tag) return {}; // Reject truncated/newer formats instead of rendering partial geometry.
    return mesh;
  };
  auto mesh=read();
  if(replacement && !mesh) {
    static std::mutex mutex;
    static std::unordered_set<std::string> logged;
    const auto name=basename(visual);
    std::lock_guard<std::mutex> guard(mutex);
    if(logged.insert(name).second)
      Tempest::Log::i("fracture: disabled for loaded mesh replacement \"",name,".MRM\": missing, invalid or stale .fracture sidecar");
    }
  return mesh;
  }

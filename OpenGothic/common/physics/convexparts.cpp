#include "convexparts.h"
#include "resources.h"
#include "utils/userhome.h"

#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Tempest/Log>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <locale>
#include <mutex>
#include <unordered_set>
#include <vector>

namespace {
std::string basename(std::string_view visual) {
  const auto slash=visual.find_last_of("/\\");
  if(slash!=std::string_view::npos) visual.remove_prefix(slash+1);
  const auto dot=visual.find_last_of('.');
  if(dot!=std::string_view::npos) visual=visual.substr(0,dot);
  std::string name(visual);
  std::transform(name.begin(),name.end(),name.begin(),[](unsigned char c) { return char(std::toupper(c)); });
  return name;
  }

std::shared_ptr<const ConvexParts> readParts(const std::filesystem::path& path,
                                            size_t sourceSize, uint32_t sourceCrc32, bool checked) {
  std::ifstream file(path);
  file.imbue(std::locale::classic());
  std::string tag;
  size_t version=0,size=0,count=0;
  uint32_t crc=0;
  if(!(file>>tag>>version) || tag!="GOTHIC_CONVEX_PARTS" || version!=1) return {};
  if(!(file>>tag>>size>>std::hex>>crc>>std::dec) || tag!="source" ||
     (checked && (size!=sourceSize || crc!=sourceCrc32))) return {};
  if(!(file>>tag>>count) || tag!="parts" || count==0 || count>16) return {};
  auto parts=std::make_shared<ConvexParts>();
  JPH::StaticCompoundShapeSettings compound;
  Physics::initialize();
  for(size_t p=0;p<count;++p) {
    size_t pointsCount=0;
    if(!(file>>tag>>pointsCount) || tag!="part" || pointsCount<4 || pointsCount>256) return {};
    std::vector<JPH::Vec3> points;
    points.reserve(pointsCount);
    for(size_t v=0;v<pointsCount;++v) {
      float x=0,y=0,z=0;
      if(!(file>>x>>y>>z) || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) ||
         std::abs(x)>1.e6f || std::abs(y)>1.e6f || std::abs(z)>1.e6f) return {};
      points.emplace_back(x*.01f,y*.01f,z*.01f);
      }
    JPH::ConvexHullShapeSettings settings(points.data(),int(points.size()),.001f);
    const auto hull=settings.Create();
    if(hull.HasError() || hull.Get()->GetVolume()<=0.f) return {};
    if(count==1) parts->shape=hull.Get();
    else compound.AddShape(JPH::Vec3::sZero(),JPH::Quat::sIdentity(),hull.Get());
    }
  if(file>>tag) return {};
  if(count>1) {
    const auto shape=compound.Create();
    if(shape.HasError()) return {};
    parts->shape=shape.Get();
    }
  parts->partCount=count;
  return parts;
  }
}

std::shared_ptr<const ConvexParts> ConvexParts::read(const std::filesystem::path& path,
                                                   size_t sourceSize, uint32_t sourceCrc32) {
  return readParts(path,sourceSize,sourceCrc32,true);
  }

std::shared_ptr<const ConvexParts> ConvexParts::load(std::string_view visual) {
  Resources::MeshReplacementSource source;
  const bool replacement=Resources::meshReplacementLoaded(visual,&source);
  std::filesystem::path path;
  if(replacement) {
    path=source.file;
    path.replace_extension(".convex-parts");
    }
  else {
    std::filesystem::path root;
    if(const auto* dir=std::getenv("OPENGOTHIC_CONVEX_PARTS")) root=dir;
    else if(const auto home=userHome(); !home.empty()) root=home/".local/share/gothic-ai/convex-parts";
    if(!root.empty()) path=root/(basename(visual)+".convex-parts");
    }
  auto parts=path.empty() ? nullptr : readParts(path,source.size,source.crc32,replacement);
  if(!parts) {
    static std::mutex mutex;
    static std::unordered_set<std::string> logged;
    const auto name=basename(visual);
    std::lock_guard<std::mutex> guard(mutex);
    if(logged.insert(name).second)
      Tempest::Log::i("furniture parts: using one hull for \"",name,"\": missing, invalid or stale .convex-parts cache");
    }
  return parts;
  }

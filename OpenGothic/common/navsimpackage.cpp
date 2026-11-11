#include "navsimpackage.h"
#include "navbench.h"
#include "sha256.h"
#include "game/knownroadsbench.h"
#include "physics/navmesh.h"
#include "world/world.h"

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
#include <rapidjson/document.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <stdexcept>

namespace {
namespace fs=std::filesystem;
std::ofstream file(const fs::path& path) {
  std::ofstream out(path);
  if(!out)throw std::runtime_error("navsim package cannot write "+path.string());
  out.exceptions(std::ios::badbit|std::ios::failbit);
  out<<std::setprecision(std::numeric_limits<double>::max_digits10)<<std::boolalpha;
  return out;
}
void vector(std::ostream& out,const NavMesh::Point& p) {
  out<<'['<<p[0]<<','<<p[1]<<','<<p[2]<<']';
}
void paths(World& world,const NavMesh& mesh,const fs::path& source,
           const char* split,std::ostream& out) {
  std::ifstream in(source);
  if(!in)throw std::runtime_error("navsim package cannot read pairs "+source.string());
  std::string line;size_t index=0;
  while(std::getline(in,line)) {
    if(line.empty())continue;
    rapidjson::Document row;row.Parse(line.data(),line.size());
    if(row.HasParseError() || !row.IsObject() || !row.HasMember("start") || !row["start"].IsString() ||
       !row.HasMember("goal") || !row["goal"].IsString())
      throw std::runtime_error("navsim package pairs require JSONL start and goal: "+source.string());
    const std::string start=row["start"].GetString(),goal=row["goal"].GetString();
    const auto* from=world.findPoint(start,false);const auto* to=world.findPoint(goal,false);
    if(!from || !to)throw std::runtime_error("navsim package missing pair point: "+start+" "+goal);
    const NavMesh::Point a={from->groundPos.x*.01f,from->groundPos.y*.01f,from->groundPos.z*.01f};
    const NavMesh::Point b={to->groundPos.x*.01f,to->groundPos.y*.01f,to->groundPos.z*.01f};
    const auto path=mesh.findPath(a,b);
    out<<"{\"split\":"<<std::quoted(split)<<",\"pair_index\":"<<index++
       <<",\"start\":"<<std::quoted(start)<<",\"goal\":"<<std::quoted(goal)<<",\"start_m\":";
    vector(out,a);out<<",\"end_m\":";vector(out,b);
    out<<",\"allow_transitions\":true,\"complete\":"<<path.complete<<",\"length_m\":"<<path.length
       <<",\"reason\":"<<std::quoted(path.reason)<<"}\n";
  }
  if(!index)throw std::runtime_error("navsim package empty pairs "+source.string());
}
std::string sha256(const fs::path& path) {
  std::ifstream input(path,std::ios::binary);
  if(!input)throw std::runtime_error("navsim package cannot hash "+path.string());
  Sha256 sha;std::array<char,65536> buffer{};
  while(input.read(buffer.data(),std::streamsize(buffer.size())) || input.gcount())
    sha.update(buffer.data(),size_t(input.gcount()));
  if(!input.eof())throw std::runtime_error("navsim package hash read failed "+path.string());
  return sha.finish();
}
}

void NavSimPackage::write(const std::vector<std::string>& args,World& world,const std::string& output) {
  if(args.size()<8)
    throw std::runtime_error("navdemos dump-package REVISION PLACES TRAIN VALIDATION TEST NPC...");
  const fs::path directory=fs::path(output)/"package";
  if(fs::exists(directory))throw std::runtime_error("navsim package output already exists");
  fs::create_directories(directory);
  const auto& mesh=NavBench::navigationMesh(world);
  if(!mesh.stats().complete)throw std::runtime_error("navsim package requires a complete navmesh");
  fs::copy_file(mesh.stats().cacheFile,directory/"navmesh.navmesh");
  fs::copy_file(args[3],directory/"places.json");
  const std::vector<std::string> actors(args.begin()+7,args.end());
  KnownRoadsBench::writePackage(world,args[3],actors,directory.string());
  {
    auto out=file(directory/"links.json");out<<'[';
    const auto& links=mesh.params().links;
    for(size_t i=0;i<links.size();++i) {
      const auto& link=links[i];if(i)out<<',';
      out<<"{\"start_m\":";vector(out,link.start);out<<",\"end_m\":";vector(out,link.end);
      out<<",\"radius_m\":"<<link.radius<<",\"type\":"<<unsigned(link.type)
         <<",\"bidirectional\":"<<link.bidirectional<<",\"id\":"<<link.id<<'}';
    }
    out<<"]\n";
  }
  {
    auto out=file(directory/"navmesh-params.json");
    out<<"{\"nearest_horizontal_m\":"<<mesh.params().nearestHorizontal
       <<",\"nearest_vertical_m\":"<<mesh.params().nearestVertical<<"}\n";
  }
  {
    auto out=file(directory/"path-checks.jsonl");
    const char* splits[]={"train","validation","test"};
    for(size_t i=0;i<3;++i) {
      fs::copy_file(args[4+i],directory/(std::string(splits[i])+".jsonl"));
      paths(world,mesh,args[4+i],splits[i],out);
    }
  }
  auto out=file(directory/"manifest.json");
  out<<"{\"format\":\"navsim-package-v1\",\"world\":"<<std::quoted(std::string(world.name()))
     <<",\"engine_revision\":"<<std::quoted(args[2])<<",\"files\":[";
  const char* names[]={"navmesh.navmesh","links.json","navmesh-params.json","roads-graph.json","actors.jsonl",
                       "places.json","train.jsonl","validation.jsonl","test.jsonl","path-checks.jsonl"};
  for(size_t i=0;i<std::size(names);++i) {
    const auto path=directory/names[i];if(i)out<<',';
    out<<"{\"path\":"<<std::quoted(names[i])<<",\"size\":"<<fs::file_size(path)
       <<",\"sha256\":"<<std::quoted(sha256(path))<<'}';
  }
  out<<"]}\n";
}

// Offline analysis only: native road knowledge and saved Detour walkable surfaces.
// Build with -std=c++17 -DDT_POLYREF64, knownroads.cpp, and Detour/Source/*.cpp;
// include common, Tempest/Engine/include, rapidjson/include, and Detour/Include.
#include "game/knownroads.h"

#include <DetourAlloc.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshQuery.h>

#include <rapidjson/document.h>
#include <rapidjson/ostreamwrapper.h>
#include <rapidjson/writer.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
using Json = rapidjson::Value;
using Writer = rapidjson::Writer<rapidjson::OStreamWrapper>;
using Mesh = std::unique_ptr<dtNavMesh, decltype(&dtFreeNavMesh)>;

rapidjson::Document parse(const std::string& text) {
  rapidjson::Document result;
  result.Parse(text.data(), text.size());
  if(result.HasParseError()) throw std::runtime_error("invalid input JSON");
  return result;
}

rapidjson::Document readJson(const std::string& path) {
  std::ifstream in(path);
  if(!in) throw std::runtime_error("cannot read JSON: " + path);
  return parse({std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()});
}

Tempest::Vec3 vector(const Json& value) {
  if(!value.IsArray() || value.Size()!=3) throw std::runtime_error("expected XYZ coordinates");
  for(const auto& axis:value.GetArray())
    if(!axis.IsNumber() || !std::isfinite(axis.GetDouble())) throw std::runtime_error("invalid coordinate");
  return {value[0].GetFloat(), value[1].GetFloat(), value[2].GetFloat()};
}

void indices(Writer& out, const std::vector<uint8_t>& mask) {
  out.StartArray();
  for(size_t i=0; i<mask.size(); ++i) if(mask[i]) out.Uint64(i);
  out.EndArray();
}

template<class T> void read(std::istream& in,T& value);

uint64_t coordinateKey(int x,int z) {return (uint64_t(uint32_t(x))<<32)|uint32_t(z);}
std::string upper(std::string text) {for(char& c:text) if(c>='a' && c<='z') c=char(c-'a'+'A');return text;}
float segmentDistanceSquared(const Tempest::Vec3& p,const Tempest::Vec3& a,const Tempest::Vec3& b) {
  const auto ab=b-a,ap=p-a;const float length=ab.quadLength();
  const float t=length>0?std::clamp((ap.x*ab.x+ap.y*ab.y+ap.z*ab.z)/length,0.f,1.f):0.f;
  return (p-a-ab*t).quadLength();
}

// Matches knownroadsbench.cpp::FamiliarArea, including 3D locality and source
// lineage. This geometry selects a mask, never a navigation class or height.
struct FamiliarArea {
  using Vec3 = Tempest::Vec3;
  struct Shape {Vec3 a,b;float radius;bool route;};
  std::vector<Shape> shapes;
  std::unordered_map<uint64_t,std::vector<size_t>> bins;
  void add(const Vec3& a,const Vec3& b,float radius,bool route) {
    const size_t id=shapes.size();shapes.push_back({a,b,radius,route});
    const int x0=int(std::floor((std::min(a.x,b.x)-radius)/2000.f)),x1=int(std::floor((std::max(a.x,b.x)+radius)/2000.f));
    const int z0=int(std::floor((std::min(a.z,b.z)-radius)/2000.f)),z1=int(std::floor((std::max(a.z,b.z)+radius)/2000.f));
    for(int z=z0;z<=z1;++z) for(int x=x0;x<=x1;++x) bins[coordinateKey(x,z)].push_back(id);
  }
  bool contains(const Vec3& point,bool source) const {
    const auto it=bins.find(coordinateKey(int(std::floor(point.x/2000.f)),int(std::floor(point.z/2000.f))));
    if(it==bins.end()) return false;
    for(auto id:it->second) {
      const auto& shape=shapes[id];
      if(source && shape.route) {
        auto p=point,a=shape.a,b=shape.b;p.y=a.y=b.y=0;const auto delta=b-a;
        const float t=delta.quadLength()>0?std::clamp(((p-a).x*delta.x+(p-a).z*delta.z)/delta.quadLength(),0.f,1.f):0.f;
        const float routeY=shape.a.y+(shape.b.y-shape.a.y)*t;
        if(segmentDistanceSquared(p,a,b)<=25.f && std::abs(point.y-routeY)<=200.f) return true;
      } else if(segmentDistanceSquared(point,shape.a,shape.b)<=shape.radius*shape.radius) return true;
    }
    return false;
  }
  size_t eligibleTiles() const {
    std::set<uint64_t> result;
    for(const auto& shape:shapes) {
      const int x0=int(std::floor((std::min(shape.a.x,shape.b.x)-shape.radius)/200.f));
      const int x1=int(std::floor((std::max(shape.a.x,shape.b.x)+shape.radius)/200.f));
      const int z0=int(std::floor((std::min(shape.a.z,shape.b.z)-shape.radius)/200.f));
      const int z1=int(std::floor((std::max(shape.a.z,shape.b.z)+shape.radius)/200.f));
      auto a=shape.a,b=shape.b;a.y=b.y=0;
      for(int z=z0;z<=z1;++z) for(int x=x0;x<=x1;++x)
        if(segmentDistanceSquared(Vec3((float(x)+.5f)*200.f,0,(float(z)+.5f)*200.f),a,b)<=shape.radius*shape.radius)
          result.insert(coordinateKey(x,z));
    }
    return result.size();
  }
};

struct FamiliarInputs {
  struct Tile {int32_t x=0,z=0;float referenceHeightCm=0;uint32_t source=0;};
  std::vector<Tempest::Vec3> sources;
  std::vector<Tile> tiles;
  std::unordered_map<std::string,Tempest::Vec3> points;
  rapidjson::Document places;

  FamiliarInputs(const std::string& placesPath,const std::string& atlasPath) {
    if(atlasPath.empty()) return;
    places=readJson(placesPath);
    std::ifstream in(atlasPath,std::ios::binary);std::array<char,8> magic{};
    if(!in.read(magic.data(),8) || std::memcmp(magic.data(),"NAVFAM4\n",8)!=0) throw std::runtime_error("invalid familiar atlas magic");
    uint32_t size=0,sourceCount=0,tileCount=0;read(in,size);
    if(size>4096) throw std::runtime_error("invalid familiar world name size");
    std::string world(size,'\0');
    if(!in.read(world.data(),size)) throw std::runtime_error("truncated familiar world name");
    if(places.HasMember("world") && upper(world)!=upper(places["world"].GetString())) throw std::runtime_error("familiar world differs from places");
    read(in,sourceCount);read(in,tileCount);
    if(sourceCount>10000000 || tileCount>10000000) throw std::runtime_error("invalid familiar atlas size");
    sources.resize(sourceCount);tiles.resize(tileCount);
    for(auto& source:sources) {read(in,source.x);read(in,source.y);read(in,source.z);}
    for(auto& tile:tiles) {
      float groundHeight=0;uint8_t channels=0;
      read(in,tile.x);read(in,tile.z);read(in,tile.referenceHeightCm);read(in,groundHeight);read(in,tile.source);read(in,channels);
      if(tile.source>=sources.size()) throw std::runtime_error("invalid familiar tile source");
    }
    if(in.peek()!=std::char_traits<char>::eof()) throw std::runtime_error("trailing data in familiar atlas");
  }

  void mask(const KnownRoads& graph,const KnownRoads::Knowledge& knowledge,const Json& event,Writer& out) const {
    if(tiles.empty()) return;
    FamiliarArea area;size_t known=0,sourceCount=0;std::set<uint64_t> knownXZ;
    if(!knowledge.stranger) {
      const auto& nodes=graph.nodes();const auto& edges=graph.edges();
      for(size_t i=0;i<edges.size();++i) if(knowledge.edges[i]) area.add(nodes[edges[i].a].position,nodes[edges[i].b].position,800.f,true);
      for(size_t i=0;i<nodes.size();++i) if(knowledge.nodes[i]) area.add(nodes[i].position,nodes[i].position,800.f,true);
      const auto place=[&](const Tempest::Vec3& p){area.add(p,p,2000.f,false);};
      place(vector(event.HasMember("spawn_cm")?event["spawn_cm"]:event["position_cm"]));
      for(const auto& routine:event["routines"].GetArray()) {
        const std::string name=upper(routine["point"].GetString());const auto point=points.find(name);
        if(point==points.end()) throw std::runtime_error("raw routine position is unavailable: "+name);
        place(point->second);
        const auto mapping=places["points"].FindMember(name.c_str());
        if(mapping!=places["points"].MemberEnd()) {
          const auto& entry=places["places"][rapidjson::SizeType(mapping->value.GetUint64())];
          const auto anchor=entry.HasMember("anchor")?points.find(upper(entry["anchor"].GetString())):points.end();
          if(entry.HasMember("anchor") && entry["anchor"].GetStringLength()>0 && anchor==points.end())
            throw std::runtime_error("raw place anchor position is unavailable: "+std::string(entry["anchor"].GetString()));
          place(anchor!=points.end()?anchor->second:vector(entry["center"]));
        }
      }
    }
    std::vector<uint8_t> eligible(sources.size(),0);
    if(!knowledge.stranger) for(size_t i=0;i<sources.size();++i) sourceCount+=eligible[i]=uint8_t(area.contains(sources[i],true));
    out.Key("known_tile_ids");out.StartArray();
    for(size_t i=0;i<tiles.size();++i) {
      const auto& tile=tiles[i];
      if(eligible[tile.source] && area.contains({(float(tile.x)+.5f)*200.f,tile.referenceHeightCm,(float(tile.z)+.5f)*200.f},false)) {
        out.Uint64(i);++known;knownXZ.insert(coordinateKey(tile.x,tile.z));
      }
    }
    out.EndArray();
    const size_t total=area.eligibleTiles();
    const std::pair<const char*,size_t> counts[]={{"known_tiles",known},{"known_2m_tiles_xz",knownXZ.size()},
      {"eligible_2m_tiles",total},{"familiar_source_poses",sourceCount}};
    for(const auto& field:counts) {
      if(event.HasMember("native_stats") && event["native_stats"][field.first].GetUint64()!=field.second)
        throw std::runtime_error(std::string("familiar mask differs from native ")+field.first+" for "+event["instance"].GetString()+
          ": got "+std::to_string(field.second)+", expected "+std::to_string(event["native_stats"][field.first].GetUint64()));
      out.Key(field.first);out.Uint64(field.second);
    }
  }
};

void roads(const std::string& graphPath, const std::string& eventsPath,FamiliarInputs familiar, Writer& out) {
  const auto graphJson = readJson(graphPath);
  if(!graphJson.IsObject() || !graphJson.HasMember("nodes") || !graphJson.HasMember("edges"))
    throw std::runtime_error("expected saved roads graph");
  std::vector<KnownRoads::Node> nodes;
  std::vector<KnownRoads::Edge> edges;
  for(const auto& node:graphJson["nodes"].GetArray()) {
    if(!node.IsArray() || node.Size()!=4 || !node[0].IsString()) throw std::runtime_error("invalid road node");
    nodes.push_back({node[0].GetString(), {node[1].GetFloat(), node[2].GetFloat(), node[3].GetFloat()}});
  }
  for(const auto& edge:graphJson["edges"].GetArray()) {
    if(!edge.IsArray() || edge.Size()!=4) throw std::runtime_error("invalid road edge");
    edges.push_back({size_t(edge[0].GetUint64()), size_t(edge[1].GetUint64()), edge[2].GetFloat(), edge[3].GetBool()});
  }
  KnownRoads graph(std::move(nodes), std::move(edges));
  // points-v3 stores groundPos; only roads-graph preserves the raw positions
  // used by FamiliarArea. This campaign's routines and anchors are graph
  // nodes. Unexported raw freepoint positions are rejected explicitly.
  for(const auto& node:graph.nodes()) familiar.points[node.name]=node.position;
  std::ifstream in(eventsPath);
  if(!in) throw std::runtime_error("cannot read road events: " + eventsPath);
  const auto actor = [&](const Json& event) {
    const char* positionKey = event.HasMember("spawn_cm") ? "spawn_cm" : "position_cm";
    const auto spawn = vector(event[positionKey]);
    std::vector<std::pair<int64_t,size_t>> routines;
    for(const auto& routine:event["routines"].GetArray())
      if(!routine["node"].IsNull()) routines.emplace_back(routine["start_ms"].GetInt64(),size_t(routine["node"].GetUint64()));
    std::stable_sort(routines.begin(),routines.end(),[](const auto& a,const auto& b){return a.first<b.first;});
    std::vector<size_t> routineNodes;
    for(const auto& routine:routines) routineNodes.push_back(routine.second);
    const bool stranger = event.HasMember("stranger") && event["stranger"].GetBool();
    const auto knowledge = graph.build(routineNodes,spawn,stranger);
    // Saved counts provide an independent identity check against the live run.
    for(const auto& field:{std::make_pair("known_nodes",&knowledge.nodes),std::make_pair("known_edges",&knowledge.edges)})
      if(event.HasMember(field.first) && event[field.first].GetUint64()!=size_t(std::count(field.second->begin(),field.second->end(),uint8_t(1))))
        throw std::runtime_error(std::string("native knowledge differs from saved ")+field.first);
    out.StartObject();
    for(const char* key:{"id","key","case_id","mode","cohort","actor","instance","npc_id"})
      if(event.HasMember(key)) {out.Key(key);event[key].Accept(out);}
    out.Key("spawn_cm");event[positionKey].Accept(out);
    out.Key("routines");event["routines"].Accept(out);
    out.Key("stranger");out.Bool(knowledge.stranger);
    out.Key("known_node_ids");indices(out,knowledge.nodes);
    out.Key("known_edge_ids");indices(out,knowledge.edges);
    familiar.mask(graph,knowledge,event,out);
    out.EndObject();
  };
  out.StartObject();out.Key("actors");out.StartArray();
  std::string line;
  while(std::getline(in,line)) {
    if(line.empty()) continue;
    const auto event = parse(line);
    if(!event.IsObject() || !event.HasMember("kind") || !event["kind"].IsString()) throw std::runtime_error("invalid roads event");
    const std::string kind = event["kind"].GetString();
    if(kind!="roads_enabled") throw std::runtime_error("expected roads_enabled event");
    actor(event);
  }
  out.EndArray();out.EndObject();
}

template<class T> void read(std::istream& in,T& value) {
  if(!in.read(reinterpret_cast<char*>(&value),sizeof(value))) throw std::runtime_error("truncated navmesh cache");
}

uint64_t hash(const unsigned char* data,size_t size) {
  uint64_t result = 14695981039346656037ull;
  for(size_t i=0; i<size; ++i) {result^=data[i];result*=1099511628211ull;}
  return result;
}

Mesh loadMesh(const std::string& path) {
  std::ifstream in(path,std::ios::binary);
  if(!in) throw std::runtime_error("cannot read navmesh cache: " + path);
  std::array<char,8> magic{};uint32_t version=0,endian=0;uint64_t key=0,count=0;double buildSeconds=0;
  dtNavMeshParams params{};
  if(!in.read(magic.data(),8) || std::memcmp(magic.data(),"OGNAV001",8)!=0) throw std::runtime_error("invalid navmesh cache magic");
  read(in,version);read(in,endian);read(in,key);read(in,buildSeconds);read(in,params);read(in,count);
  if(version!=2 || endian!=0x01020304u) throw std::runtime_error("unsupported navmesh cache format");
  if(params.maxTiles<=0 || params.maxTiles>(1<<20) || params.maxPolys<=0 || params.maxPolys>(1<<20) ||
     count>uint64_t(params.maxTiles) || !std::isfinite(params.tileWidth) || params.tileWidth<=0 ||
     !std::isfinite(params.tileHeight) || params.tileHeight<=0) throw std::runtime_error("invalid navmesh cache parameters");
  Mesh mesh(dtAllocNavMesh(),dtFreeNavMesh);
  if(!mesh || dtStatusFailed(mesh->init(&params))) throw std::runtime_error("cannot initialize Detour mesh");
  for(uint64_t i=0; i<count; ++i) {
    dtTileRef reference=0;uint32_t size=0;uint64_t checksum=0;
    read(in,reference);read(in,size);read(in,checksum);
    if(size<sizeof(dtMeshHeader) || size>64u*1024u*1024u) throw std::runtime_error("invalid navmesh tile size");
    std::unique_ptr<unsigned char,decltype(&dtFree)> data(static_cast<unsigned char*>(dtAlloc(size,DT_ALLOC_PERM)),dtFree);
    if(!data) throw std::runtime_error("cannot allocate navmesh tile");
    if(!in.read(reinterpret_cast<char*>(data.get()),size) || hash(data.get(),size)!=checksum) throw std::runtime_error("navmesh tile checksum mismatch");
    const auto* header = reinterpret_cast<const dtMeshHeader*>(data.get());
    if(header->magic!=DT_NAVMESH_MAGIC || header->version!=DT_NAVMESH_VERSION || header->polyCount<0)
      throw std::runtime_error("invalid Detour tile header");
    if(dtStatusFailed(mesh->addTile(data.get(),int(size),DT_TILE_FREE_DATA,reference,nullptr))) throw std::runtime_error("cannot add Detour tile");
    data.release();
  }
  if(in.peek()!=std::char_traits<char>::eof()) throw std::runtime_error("trailing data in navmesh cache");
  // Exact native cache-v2 compatibility: bit 2 selects ordinary ground only.
  for(int i=0; i<mesh->getMaxTiles(); ++i) {
    const auto* tile = static_cast<const dtNavMesh&>(*mesh).getTile(i);
    if(!tile || !tile->header) continue;
    for(int j=0; j<tile->header->polyCount; ++j)
      if(tile->polys[j].getType()==DT_POLYTYPE_GROUND)
        mesh->setPolyFlags(mesh->getPolyRefBase(tile)|dtPolyRef(j),tile->polys[j].flags|2);
  }
  return mesh;
}

void point(Writer& out,const float* values) {
  out.StartArray();for(size_t axis=0;axis<3;++axis) out.Double(double(values[axis])*100.);out.EndArray();
}

void queryPath(dtNavMeshQuery& query,const Json& input,Writer& out) {
  constexpr int maxPath = 32768;
  const auto startCm = vector(input["start_cm"]),goalCm = vector(input["goal_cm"]);
  const float start[]={startCm.x*.01f,startCm.y*.01f,startCm.z*.01f};
  const float goal[]={goalCm.x*.01f,goalCm.y*.01f,goalCm.z*.01f};
  float snappedStart[3]{},snappedGoal[3]{};
  const float extents[]={1.f,2.f,1.f};dtPolyRef startRef=0,goalRef=0;dtQueryFilter filter;filter.setIncludeFlags(2);
  std::string reason="ok";std::vector<float> corners(size_t(maxPath)*3);int cornerCount=0;
  if(dtStatusFailed(query.findNearestPoly(start,extents,&filter,&startRef,snappedStart)) || !startRef) reason="start_off_mesh";
  else if(dtStatusFailed(query.findNearestPoly(goal,extents,&filter,&goalRef,snappedGoal)) || !goalRef) reason="end_off_mesh";
  else {
    std::vector<dtPolyRef> corridor(maxPath);int count=0;
    const auto status=query.findPath(startRef,goalRef,snappedStart,snappedGoal,&filter,corridor.data(),&count,maxPath);
    if(dtStatusFailed(status) || dtStatusDetail(status,DT_OUT_OF_NODES) || dtStatusDetail(status,DT_BUFFER_TOO_SMALL)) reason="query_capacity";
    else if(count==0 || corridor[size_t(count-1)]!=goalRef || dtStatusDetail(status,DT_PARTIAL_RESULT)) reason="disconnected";
    else {
      std::vector<unsigned char> flags(maxPath);
      const auto straight=query.findStraightPath(snappedStart,snappedGoal,corridor.data(),count,corners.data(),flags.data(),nullptr,&cornerCount,maxPath);
      if(dtStatusFailed(straight) || dtStatusDetail(straight,DT_BUFFER_TOO_SMALL) || cornerCount==0 ||
         !(flags[size_t(cornerCount-1)]&DT_STRAIGHTPATH_END)) reason="string_pull_failed";
    }
  }
  double length=0;
  if(reason=="ok") for(int i=1;i<cornerCount;++i) {
    double squared=0;
    for(size_t axis=0;axis<3;++axis) {const double d=double(corners[size_t(i)*3+axis])-corners[size_t(i-1)*3+axis];squared+=d*d;}
    length+=std::sqrt(squared);
  }
  out.StartObject();
  if(input.HasMember("id")) {out.Key("id");input["id"].Accept(out);}
  out.Key("complete");out.Bool(reason=="ok");out.Key("reason");out.String(reason.c_str());
  out.Key("length_m");out.Double(length);out.Key("corners_cm");out.StartArray();
  if(reason=="ok") for(int i=0;i<cornerCount;++i) point(out,corners.data()+size_t(i)*3);
  out.EndArray();out.EndObject();
}

void mesh(const std::string& cache,const std::string& queries,Writer& out) {
  auto mesh = loadMesh(cache);
  out.StartObject();out.Key("units");out.String("cm");out.Key("geometry");out.String("Detour walkable surface, ground polygons only");
  out.Key("polygons_cm");out.StartArray();
  for(int i=0;i<mesh->getMaxTiles();++i) {
    const auto* tile = static_cast<const dtNavMesh&>(*mesh).getTile(i);
    if(!tile || !tile->header) continue;
    for(int j=0;j<tile->header->polyCount;++j) {
      const auto& polygon=tile->polys[j];
      if(polygon.getType()!=DT_POLYTYPE_GROUND || !(polygon.flags&1)) continue;
      out.StartArray();for(unsigned int v=0;v<polygon.vertCount;++v) point(out,tile->verts+size_t(polygon.verts[v])*3);out.EndArray();
    }
  }
  out.EndArray();out.Key("queries");out.StartArray();
  if(!queries.empty()) {
    const auto input = readJson(queries);
    if(!input.IsArray()) throw std::runtime_error("expected an array of path queries");
    std::unique_ptr<dtNavMeshQuery,decltype(&dtFreeNavMeshQuery)> query(dtAllocNavMeshQuery(),dtFreeNavMeshQuery);
    if(!query || dtStatusFailed(query->init(mesh.get(),65535))) throw std::runtime_error("cannot initialize Detour query");
    for(const auto& item:input.GetArray()) queryPath(*query,item,out);
  }
  out.EndArray();out.EndObject();
}
}

int main(int argc,char** argv) {
  try {
    if(argc<2) throw std::runtime_error("expected roads or mesh mode");
    std::string graph,events,cache,queries,output,places,atlas;
    for(int i=2;i<argc;++i) {
      const std::string flag=argv[i];
      if(i+1>=argc) throw std::runtime_error("missing argument for "+flag);
      const std::string value=argv[++i];
      if(flag=="--graph") graph=value;
      else if(flag=="--events") events=value;
      else if(flag=="--cache") cache=value;
      else if(flag=="--queries") queries=value;
      else if(flag=="--output") output=value;
      else if(flag=="--places") places=value;
      else if(flag=="--atlas") atlas=value;
      else throw std::runtime_error("unknown option: "+flag);
    }
    if(output.empty()) throw std::runtime_error("--output is required");
    std::ofstream file(output);
    if(!file) throw std::runtime_error("cannot write output: "+output);
    rapidjson::OStreamWrapper stream(file);Writer out(stream);
    const std::string mode=argv[1];
    if(mode=="roads" && !graph.empty() && !events.empty()) {
      if(!atlas.empty() && places.empty()) throw std::runtime_error("--atlas requires --places");
      roads(graph,events,FamiliarInputs(places,atlas),out);
    }
    else if(mode=="mesh" && !cache.empty()) mesh(cache,queries,out);
    else throw std::runtime_error("roads requires --graph/--events; mesh requires --cache");
    file << '\n';file.close();
    if(!file) throw std::runtime_error("cannot finish output: "+output);
    return 0;
  } catch(const std::exception& error) {
    std::cerr << "nav-memory-v4 geometry: " << error.what() << '\n';
    return 1;
  }
}

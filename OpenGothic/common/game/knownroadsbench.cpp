#include "knownroadsbench.h"
#include "knownroads.h"
#include "navigationeyebench.h"
#include "navigationmemory.h"

#include "game/gamescript.h"
#include "physics/dynamicworld.h"
#include "world/world.h"
#include "world/objects/npc.h"

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
#include <rapidjson/document.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace {
using Tempest::Vec3;
using Clock=std::chrono::steady_clock;

struct Place {
  std::string id,kind,anchor;
  Vec3 center;
};
struct Routine {
  std::string point;
  int64_t start=0,end=0;
  size_t node=KnownRoads::invalid;
};
struct Actor {
  Npc* npc=nullptr;
  int64_t handleId=0;
  std::string selector;
  Vec3 spawn;
  std::vector<Routine> routines;
  std::vector<Vec3> destinations;
  KnownRoads::Knowledge knowledge;
  KnownRoads::Path lastPath;
  bool hasPath=false;
};
struct State {
  World* world=nullptr;
  std::string worldName,directory;
  std::vector<Place> places;
  std::unordered_map<std::string,size_t> pointPlace;
  std::unordered_map<std::string,size_t> nodeByName;
  std::unique_ptr<KnownRoads> graph;
  std::vector<Actor> actors;
  std::unique_ptr<NavigationMemory::FamiliarMap> familiar;
  std::vector<Vec3> familiarSources;
} state;

std::string quote(std::string_view value) {
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  writer.String(value.data(),rapidjson::SizeType(value.size()));
  return {buffer.GetString(),buffer.GetSize()};
}
std::string number(double value) {
  if(!std::isfinite(value)) return "null";
  std::ostringstream out;
  out << std::setprecision(10) << value;
  return out.str();
}
std::string vector(const Vec3& p) {
  return '['+number(p.x)+','+number(p.y)+','+number(p.z)+']';
}
std::string upper(std::string text) {
  for(char& c:text) if(c>='a' && c<='z') c=char(c-'a'+'A');
  return text;
}
bool digits(std::string_view value) {
  return !value.empty() && std::all_of(value.begin(),value.end(),[](char c){return c>='0' && c<='9';});
}
void append(const char* file,const std::string& row) {
  std::ofstream out(state.directory+'/'+file,std::ios::app);
  if(!out) throw std::runtime_error(std::string("cannot open roads journal ")+file);
  out << row << '\n';
  if(!out) throw std::runtime_error(std::string("cannot write roads journal ")+file);
}
std::string symbol(World& world,const Npc& npc) {
  const auto* sym=world.script().findSymbol(size_t(npc.instanceSymbol()));
  return sym?std::string(sym->name()):std::string{};
}
Npc& resolve(World& world,const std::string& selector) {
  Npc* npc=nullptr;
  if(selector=="player") npc=world.player();
  else if(digits(selector)) npc=NavigationEyeBench::actor(std::stoull(selector));
  else if(selector.starts_with("id:") && digits(selector.substr(3))) {
    const auto id=std::stoull(selector.substr(3));
    if(id<=std::numeric_limits<uint32_t>::max()) npc=world.npcById(uint32_t(id));
  } else {
    const auto sym=world.script().findSymbolIndex(upper(selector));
    if(sym!=size_t(-1)) npc=world.findNpcByInstance(sym);
  }
  if(!npc) throw std::runtime_error("roads actor unavailable: "+selector);
  return *npc;
}
bool live(const Actor& actor,World& world) {
  // NPC array indices are transient; validate identity before dereferencing.
  for(uint32_t id=0;id<world.npcCount();++id)
    if(auto* npc=world.npcById(id);npc==actor.npc) return npc->handle().id==actor.handleId;
  return false;
}
void requireGraph() {
  if(!state.graph) throw std::runtime_error("roads load <places.json> is required first");
}
size_t nodeFor(World& world,std::string_view name) {
  const auto it=state.nodeByName.find(upper(std::string(name)));
  if(it!=state.nodeByName.end()) return it->second;
  // TA routinely names a freepoint. It maps onto the nearest real graph node,
  // while the freepoint itself is never inserted into the road graph.
  if(const auto* point=world.findPoint(upper(std::string(name)),false)) return state.graph->nearest(point->position());
  return KnownRoads::invalid;
}
std::vector<Routine> routines(World& world,const Npc& npc) {
  std::vector<Routine> result;
  npc.forEachRoutine([&](const Npc::RoutineInfo& r){
    result.push_back({std::string(r.point),r.start.toInt(),r.end.toInt(),nodeFor(world,r.point)});
  });
  std::stable_sort(result.begin(),result.end(),[](const Routine& a,const Routine& b){return a.start<b.start;});
  return result;
}
std::vector<size_t> routineNodes(const std::vector<Routine>& points) {
  std::vector<size_t> result;
  for(const auto& point:points) if(point.node!=KnownRoads::invalid) result.push_back(point.node);
  return result;
}
std::string routineJson(const std::vector<Routine>& points) {
  std::ostringstream out;out << '[';
  for(size_t i=0;i<points.size();++i) {
    if(i) out << ',';
    const auto& r=points[i];
    out << "{\"point\":" << quote(r.point) << ",\"start_ms\":" << r.start << ",\"end_ms\":" << r.end
        << ",\"node\":" << (r.node==KnownRoads::invalid?"null":std::to_string(r.node)) << '}';
  }
  out << ']';return out.str();
}
size_t count(const std::vector<uint8_t>& mask) {
  return size_t(std::count_if(mask.begin(),mask.end(),[](uint8_t bit){return bit!=0;}));
}
std::string stats(const KnownRoads::Knowledge& k) {
  const auto n=count(k.nodes),e=count(k.edges);
  return "\"known_nodes\":"+std::to_string(n)+",\"known_edges\":"+std::to_string(e)+
      ",\"total_nodes\":"+std::to_string(k.nodes.size())+",\"total_edges\":"+std::to_string(k.edges.size())+
      ",\"node_fraction\":"+number(k.nodes.empty()?0:double(n)/double(k.nodes.size()))+
      ",\"edge_fraction\":"+number(k.edges.empty()?0:double(e)/double(k.edges.size()))+
      ",\"stranger\":"+(k.stranger?"true":"false");
}
std::string maskJson(const std::vector<uint8_t>& mask) {
  std::ostringstream out;out << '[';bool first=true;
  for(size_t i=0;i<mask.size();++i) if(mask[i]) {if(!first) out << ',';first=false;out << i;}
  out << ']';return out.str();
}
std::string indices(const std::vector<size_t>& values) {
  std::ostringstream out;out << '[';
  for(size_t i=0;i<values.size();++i) {if(i) out << ',';out << values[i];}
  out << ']';return out.str();
}
std::string pathJson(const KnownRoads::Path& p,const KnownRoads::Knowledge& k) {
  size_t unknown=0;
  for(auto edge:p.edges) if(edge>=k.edges.size() || !k.edges[edge]) ++unknown;
  std::ostringstream out;
  out << "{\"nodes\":" << indices(p.nodes) << ",\"edges\":" << indices(p.edges)
      << ",\"length_m\":" << number(p.lengthCm*.01) << ",\"remaining_m\":" << number(p.remainingCm*.01)
      << ",\"start_offset_m\":" << number(p.startOffsetCm*.01) << ",\"direction\":" << vector(p.direction)
      << ",\"goal_in_known\":" << (p.goalInKnown?"true":"false") << ",\"unknown_edge_count\":" << unknown;
  if(!p.nodes.empty()) {
    out << ",\"endpoint\":" << quote(state.graph->nodes()[p.nodes.back()].name)
        << ",\"endpoint_cm\":" << vector(state.graph->nodes()[p.nodes.back()].position);
  }
  out << '}';return out.str();
}

void graphJournal() {
  std::ofstream out(state.directory+"/roads-graph.json",std::ios::trunc);
  if(!out) throw std::runtime_error("cannot open roads graph journal");
  out << std::setprecision(9) << "{\"kind\":\"roads_graph\",\"world\":" << quote(state.worldName)
      << ",\"node_columns\":[\"name\",\"x_cm\",\"y_cm\",\"z_cm\"],\"nodes\":[";
  const auto& nodes=state.graph->nodes();
  for(size_t i=0;i<nodes.size();++i) {
    if(i) out << ',';
    const auto& node=nodes[i];
    out << '[' << quote(node.name) << ',' << node.position.x << ',' << node.position.y << ',' << node.position.z << ']';
  }
  out << "],\"edge_columns\":[\"a\",\"b\",\"length_cm\",\"major\"],\"edges\":[";
  const auto& edges=state.graph->edges();
  for(size_t i=0;i<edges.size();++i) {
    if(i) out << ',';
    const auto& edge=edges[i];
    out << '[' << edge.a << ',' << edge.b << ',' << edge.lengthCm << ',' << (edge.major?"true":"false") << ']';
  }
  out << "]}\n";
  if(!out) throw std::runtime_error("cannot write roads graph journal");
}
std::string load(World& world,const std::string& file) {
  std::ifstream in(file);
  if(!in) throw std::runtime_error("cannot read roads places: "+file);
  const std::string data((std::istreambuf_iterator<char>(in)),std::istreambuf_iterator<char>());
  rapidjson::Document json;json.Parse(data.data(),data.size());
  if(json.HasParseError() || !json.IsObject() || !json.HasMember("places") || !json["places"].IsArray() ||
     !json.HasMember("points") || !json["points"].IsObject()) throw std::runtime_error("invalid roads places book");
  if(json.HasMember("world") && json["world"].IsString() && upper(json["world"].GetString())!=upper(std::string(world.name())))
    throw std::runtime_error("roads places book is for a different world");
  std::vector<Place> places;
  for(const auto& p:json["places"].GetArray()) {
    if(!p.IsObject() || !p.HasMember("id") || !p["id"].IsString() || !p.HasMember("kind") || !p["kind"].IsString() ||
       !p.HasMember("center") || !p["center"].IsArray() || p["center"].Size()!=3)
      throw std::runtime_error("invalid roads place entry");
    Place place;place.id=p["id"].GetString();place.kind=p["kind"].GetString();
    for(size_t i=0;i<3;++i) if(!p["center"][rapidjson::SizeType(i)].IsNumber()) throw std::runtime_error("invalid roads place center");
    place.center=Vec3(p["center"][0].GetFloat(),p["center"][1].GetFloat(),p["center"][2].GetFloat());
    if(p.HasMember("anchor") && p["anchor"].IsString()) place.anchor=p["anchor"].GetString();
    places.push_back(std::move(place));
  }
  std::unordered_map<std::string,size_t> pointPlace;
  for(auto it=json["points"].MemberBegin();it!=json["points"].MemberEnd();++it) {
    if(!it->value.IsUint64() || it->value.GetUint64()>=places.size()) throw std::runtime_error("invalid roads point place index");
    pointPlace.emplace(upper(it->name.GetString()),size_t(it->value.GetUint64()));
  }
  std::vector<const WayPoint*> waypoints;
  world.forEachWayPoint([&](const WayPoint& point){if(!point.isFreePoint()) waypoints.push_back(&point);});
  std::sort(waypoints.begin(),waypoints.end(),[](const WayPoint* a,const WayPoint* b){return a->name<b->name;});
  std::vector<KnownRoads::Node> nodes;
  std::unordered_map<const WayPoint*,size_t> ids;
  std::unordered_map<std::string,size_t> nodeByName;
  for(const auto* point:waypoints) {
    const auto id=nodes.size();ids.emplace(point,id);nodeByName.emplace(point->name,id);
    nodes.push_back({point->name,point->position()});
  }
  const auto major=[&](size_t node){
    const auto it=pointPlace.find(nodes[node].name);
    return it!=pointPlace.end() && (places[it->second].kind=="road" || places[it->second].kind=="street");
  };
  std::map<std::pair<size_t,size_t>,KnownRoads::Edge> unique;
  for(size_t a=0;a<waypoints.size();++a) for(const auto& connection:waypoints[a]->connections()) {
    const auto it=ids.find(connection.point);
    if(it==ids.end() || a>=it->second) continue;
    const auto b=it->second;
    unique.emplace(std::make_pair(a,b),KnownRoads::Edge{a,b,float(connection.len),major(a)&&major(b)});
  }
  std::vector<KnownRoads::Edge> edges;edges.reserve(unique.size());
  for(const auto& entry:unique) edges.push_back(entry.second);
  auto graph=std::make_unique<KnownRoads>(std::move(nodes),std::move(edges));
  state.places=std::move(places);state.pointPlace=std::move(pointPlace);state.nodeByName=std::move(nodeByName);
  state.graph=std::move(graph);state.actors.clear();graphJournal();
  size_t majorEdges=0;
  for(const auto& edge:state.graph->edges()) majorEdges+=edge.major;
  return "{\"kind\":\"roads_loaded\",\"nodes\":"+std::to_string(state.graph->nodes().size())+
      ",\"edges\":"+std::to_string(state.graph->edges().size())+",\"major_edges\":"+std::to_string(majorEdges)+
      ",\"places\":"+std::to_string(state.places.size())+"}";
}

Vec3 goal(World& world,const std::string& name) {
  const auto key=upper(name);
  if(const auto* point=world.findPoint(key,false)) return point->position();
  for(const auto& place:state.places) if(upper(place.id)==key) {
    if(!place.anchor.empty()) if(const auto* point=world.findPoint(place.anchor,false)) return point->position();
    return place.center;
  }
  throw std::runtime_error("roads unknown place or waypoint: "+name);
}
Actor& tracked(World& world,const std::string& selector) {
  auto& npc=resolve(world,selector);
  for(auto& actor:state.actors) if(actor.npc==&npc && live(actor,world)) return actor;
  throw std::runtime_error("roads on|stranger is required for actor: "+selector);
}

constexpr float FamiliarCorridorCm=800.f, FamiliarPlaceCm=2000.f;
constexpr size_t FamiliarMaxBytes=50000000;

uint64_t coordinateKey(int x,int z) {
  return (uint64_t(uint32_t(x))<<32)|uint32_t(z);
}
float segmentDistanceSquared(const Vec3& p,const Vec3& a,const Vec3& b) {
  const auto ab=b-a,ap=p-a;
  const float length=ab.quadLength();
  const float t=length>0?std::clamp((ap.x*ab.x+ap.y*ab.y+ap.z*ab.z)/length,0.f,1.f):0.f;
  return (p-a-ab*t).quadLength();
}

// The geometry here only selects legitimate observation poses and a locality
// mask. No shape below supplies a navigation class or ground height.
struct FamiliarArea {
  struct Shape {Vec3 a,b;float radius;bool route;};
  std::vector<Shape> shapes;
  std::unordered_map<uint64_t,std::vector<size_t>> bins;
  void add(const Vec3& a,const Vec3& b,float radius,bool route) {
    const size_t id=shapes.size();shapes.push_back({a,b,radius,route});
    const int x0=int(std::floor((std::min(a.x,b.x)-radius)/FamiliarPlaceCm));
    const int x1=int(std::floor((std::max(a.x,b.x)+radius)/FamiliarPlaceCm));
    const int z0=int(std::floor((std::min(a.z,b.z)-radius)/FamiliarPlaceCm));
    const int z1=int(std::floor((std::max(a.z,b.z)+radius)/FamiliarPlaceCm));
    for(int z=z0;z<=z1;++z) for(int x=x0;x<=x1;++x) bins[coordinateKey(x,z)].push_back(id);
  }
  bool contains(const Vec3& point,bool source) const {
    const auto it=bins.find(coordinateKey(int(std::floor(point.x/FamiliarPlaceCm)),int(std::floor(point.z/FamiliarPlaceCm))));
    if(it==bins.end()) return false;
    for(auto id:it->second) {
      const auto& shape=shapes[id];
      // An unknown route inside a known corridor is not an eligible eye pose.
      if(source && shape.route) {
        auto p=point,a=shape.a,b=shape.b;p.y=a.y=b.y=0;
        const auto delta=b-a;
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

FamiliarArea familiarArea(World& world,const Actor& actor) {
  FamiliarArea area;
  if(actor.knowledge.stranger) return area;
  const auto& nodes=state.graph->nodes();const auto& edges=state.graph->edges();
  for(size_t i=0;i<edges.size();++i) if(actor.knowledge.edges[i])
    area.add(nodes[edges[i].a].position,nodes[edges[i].b].position,FamiliarCorridorCm,true);
  for(size_t i=0;i<nodes.size();++i) if(actor.knowledge.nodes[i])
    area.add(nodes[i].position,nodes[i].position,FamiliarCorridorCm,true);
  const auto place=[&](const Vec3& point){area.add(point,point,FamiliarPlaceCm,false);};
  place(actor.spawn);
  for(const auto& routine:actor.routines) {
    if(const auto* point=world.findPoint(routine.point,false)) place(point->position());
    else if(routine.node!=KnownRoads::invalid) place(nodes[routine.node].position);
    const auto it=state.pointPlace.find(upper(routine.point));
    if(it!=state.pointPlace.end()) {
      const auto& known=state.places[it->second];
      const auto* anchor=known.anchor.empty()?nullptr:world.findPoint(known.anchor,false);
      place(anchor?anchor->position():known.center);
    }
  }
  for(const auto& destination:actor.destinations) place(destination);
  return area;
}

size_t familiarMaskBytes(size_t tiles) {return ((tiles+63)/64)*sizeof(uint64_t);}
size_t familiarBytes(size_t actors) {
  if(!state.familiar) return 0;
  return sizeof(*state.familiar)+state.familiar->storageBytes()+sizeof(state.familiarSources)+state.familiarSources.capacity()*sizeof(Vec3)+
      actors*(sizeof(NavigationMemory::FamiliarMask)+familiarMaskBytes(state.familiar->tiles.size()));
}
std::string familiarStats(size_t actors) {
  if(!state.familiar) throw std::runtime_error("roads bake or familiar-load is required first");
  return "\"shared_tiles\":"+std::to_string(state.familiar->tiles.size())+
      ",\"source_poses\":"+std::to_string(state.familiarSources.size())+
      ",\"shared_bytes\":"+std::to_string(sizeof(*state.familiar)+state.familiar->storageBytes())+
      ",\"source_bytes\":"+std::to_string(sizeof(state.familiarSources)+state.familiarSources.capacity()*sizeof(Vec3))+
      ",\"mask_bytes_per_npc\":"+std::to_string(sizeof(NavigationMemory::FamiliarMask)+familiarMaskBytes(state.familiar->tiles.size()))+
      ",\"npc_count\":"+std::to_string(actors)+",\"total_bytes_for_npcs\":"+std::to_string(familiarBytes(actors))+
      ",\"max_bytes\":"+std::to_string(FamiliarMaxBytes)+",\"corridor_halfwidth_m\":8,\"place_radius_m\":20";
}
NavigationMemory::FamiliarMask familiarMask(World& world,const Actor& actor,std::string* details=nullptr) {
  const auto start=Clock::now();
  auto area=familiarArea(world,actor);
  NavigationMemory::FamiliarMask mask;
  std::vector<uint8_t> eligible(state.familiarSources.size(),0);
  size_t sourceCount=0,known=0;
  std::set<uint64_t> knownXZ;
  if(!actor.knowledge.stranger) {
    mask.assign((state.familiar->tiles.size()+63)/64,0);
    for(size_t i=0;i<eligible.size();++i) sourceCount+=eligible[i]=uint8_t(area.contains(state.familiarSources[i],true));
    for(size_t i=0;i<state.familiar->tiles.size();++i) {
      const auto& tile=state.familiar->tiles[i];
      if(tile.source>=eligible.size()) throw std::runtime_error("familiar tile has an invalid observation source");
      if(!eligible[tile.source]) continue;
      const Vec3 center((float(tile.x)+.5f)*200.f,tile.referenceHeightCm,(float(tile.z)+.5f)*200.f);
      if(!area.contains(center,false)) continue;
      mask[i/64]|=uint64_t(1)<<(i%64);++known;knownXZ.insert(coordinateKey(tile.x,tile.z));
    }
  }
  const double cost=std::chrono::duration<double,std::micro>(Clock::now()-start).count();
  if(details) {
    const auto total=area.eligibleTiles();
    *details="\"known_tiles\":"+std::to_string(known)+",\"known_2m_tiles_xz\":"+std::to_string(knownXZ.size())+
        ",\"eligible_2m_tiles\":"+std::to_string(total)+",\"coverage_fraction\":"+number(total?double(knownXZ.size())/double(total):0)+
        ",\"familiar_source_poses\":"+std::to_string(sourceCount)+",\"mask_build_us\":"+number(cost);
  }
  return mask;
}

template<class T> void writeFamiliar(std::ostream& out,const T& value) {
  out.write(reinterpret_cast<const char*>(&value),sizeof(value));
}
template<class T> T readFamiliar(std::istream& in) {
  T result{};in.read(reinterpret_cast<char*>(&result),sizeof(result));
  if(!in) throw std::runtime_error("truncated familiar atlas");
  return result;
}
void requireAbsolute(const std::string& file) {
  if(!std::filesystem::path(file).is_absolute()) throw std::runtime_error("familiar atlas path must be absolute");
}
void saveFamiliar(const std::string& file) {
  requireAbsolute(file);
  std::ofstream out(file,std::ios::binary|std::ios::trunc);
  if(!out) throw std::runtime_error("cannot write familiar atlas: "+file);
  out.write("NAVFAM4\n",8);
  writeFamiliar(out,uint32_t(state.worldName.size()));out.write(state.worldName.data(),std::streamsize(state.worldName.size()));
  writeFamiliar(out,uint32_t(state.familiarSources.size()));writeFamiliar(out,uint32_t(state.familiar->tiles.size()));
  for(const auto& p:state.familiarSources) {writeFamiliar(out,p.x);writeFamiliar(out,p.y);writeFamiliar(out,p.z);}
  for(const auto& t:state.familiar->tiles) {
    writeFamiliar(out,t.x);writeFamiliar(out,t.z);writeFamiliar(out,t.referenceHeightCm);
    writeFamiliar(out,t.groundHeightCm);writeFamiliar(out,t.source);writeFamiliar(out,t.channels);
  }
  if(!out) throw std::runtime_error("cannot write familiar atlas: "+file);
}
std::string loadFamiliar(const std::string& file) {
  requireAbsolute(file);
  std::ifstream in(file,std::ios::binary);
  if(!in) throw std::runtime_error("cannot read familiar atlas: "+file);
  std::array<char,8> magic{};in.read(magic.data(),std::streamsize(magic.size()));
  if(std::string_view(magic.data(),magic.size())!="NAVFAM4\n") throw std::runtime_error("unsupported familiar atlas version");
  const auto nameSize=readFamiliar<uint32_t>(in);
  if(nameSize>4096) throw std::runtime_error("invalid familiar atlas world name");
  std::string name(nameSize,'\0');in.read(name.data(),std::streamsize(name.size()));
  if(!in || upper(name)!=upper(state.worldName)) throw std::runtime_error("familiar atlas is for a different world");
  const auto sourceCount=readFamiliar<uint32_t>(in),tileCount=readFamiliar<uint32_t>(in);
  if(size_t(sourceCount)*sizeof(Vec3)+size_t(tileCount)*sizeof(NavigationMemory::FamiliarTile)+
     300*(sizeof(NavigationMemory::FamiliarMask)+familiarMaskBytes(tileCount))>FamiliarMaxBytes)
    throw std::runtime_error("familiar atlas exceeds the 50 MB world budget for 300 NPCs");
  std::vector<Vec3> sources(sourceCount);
  for(auto& p:sources) {
    p.x=readFamiliar<float>(in);p.y=readFamiliar<float>(in);p.z=readFamiliar<float>(in);
    if(!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) throw std::runtime_error("invalid familiar source pose");
  }
  auto map=std::make_unique<NavigationMemory::FamiliarMap>();map->tiles.resize(tileCount);
  for(auto& t:map->tiles) {
    t.x=readFamiliar<int32_t>(in);t.z=readFamiliar<int32_t>(in);t.referenceHeightCm=readFamiliar<float>(in);
    t.groundHeightCm=readFamiliar<float>(in);t.source=readFamiliar<uint32_t>(in);t.channels=readFamiliar<uint8_t>(in);
    if(t.source>=sourceCount || !std::isfinite(t.referenceHeightCm) || (t.channels&~uint8_t(31)))
      throw std::runtime_error("invalid familiar tile");
  }
  map->finish();
  state.familiar=std::move(map);state.familiarSources=std::move(sources);
  if(familiarBytes(300)>FamiliarMaxBytes) throw std::runtime_error("familiar atlas exceeds the 50 MB world budget for 300 NPCs");
  return "{\"kind\":\"familiar_loaded\",\"file\":"+quote(file)+",\"file_bytes\":"+std::to_string(std::filesystem::file_size(file))+','+familiarStats(300)+"}";
}

std::string bakeFamiliar(World& world,const std::string& file) {
  requireAbsolute(file);
  if(!world.physic()) throw std::runtime_error("familiar bake requires world physics");
  struct Source {Vec3 position,previous;};
  std::vector<Source> poses;
  std::map<std::array<int64_t,3>,size_t> unique;
  const auto addPose=[&](const Vec3& p,const Vec3& previous) {
    const std::array<int64_t,3> key={std::llround(p.x),std::llround(p.y),std::llround(p.z)};
    if(unique.emplace(key,poses.size()).second) poses.push_back({p,previous});
  };
  const auto& nodes=state.graph->nodes();
  for(const auto& edge:state.graph->edges()) {
    const auto a=nodes[edge.a].position,b=nodes[edge.b].position,delta=b-a;
    const float length=delta.length();
    const size_t steps=std::max(size_t(1),size_t(std::ceil(length/200.f)));
    for(size_t i=0;i<=steps;++i) {
      const float fraction=float(i)/float(steps);
      const auto p=a+delta*fraction;
      const float previousFraction=length>0?std::clamp(fraction+(i==0?1.f:-1.f)*std::min(2.f/length,.5f),0.f,1.f):fraction;
      addPose(p,a+delta*previousFraction);
    }
  }
  for(const auto& node:nodes) addPose(node.position,node.position);
  // Routine freepoints are real authored poses, never invented floor samples.
  world.forEachWayPoint([&](const WayPoint& p){addPose(p.position(),p.position());});
  auto map=std::make_unique<NavigationMemory::FamiliarMap>();
  std::vector<Vec3> sources;sources.reserve(poses.size());
  NavigationEye::Config config;config.layout=250;
  NavigationEye eye(config);NavigationMemory memory;
  const auto* player=world.player();
  if(!player) throw std::runtime_error("familiar bake requires the human stand actor");
  const auto& guild=world.script().guildVal();
  const float step=float(guild.step_height[player->guild()]);
  const float pi=3.14159265358979323846f;
  const float maxSlope=std::acos(std::clamp(std::cos(float(guild.slide_angle[player->guild()])*pi/180.f),0.f,1.f))*180.f/pi;
  const std::array<Vec3,4> forwards={Vec3(1,0,0),Vec3(0,0,1),Vec3(-1,0,0),Vec3(0,0,-1)};
  size_t casts=0,rejected=0;const auto start=Clock::now();
  for(size_t source=0;source<poses.size();++source) {
    auto p=poses[source];
    // Physics only stages a supported eye pose, as the navigation stand does.
    // This support hit is never ingested into memory or copied to an atlas tile.
    const auto support=world.physic()->navigationRay(p.position+Vec3(0,200,0),p.position-Vec3(0,200,0),nullptr);
    ++casts;
    if(!support.hasCol || (support.kind!=DynamicWorld::NavigationHit::World && support.kind!=DynamicWorld::NavigationHit::StaticVob) ||
       support.normal.y<NavigationMemory::Constants::groundNormalY || std::abs(support.point.y-p.position.y)>200.f) {
      ++rejected;continue;
    }
    p.position.y=p.previous.y=support.point.y;
    memory.reset();sources.push_back(p.position);
    for(size_t yaw=0;yaw<forwards.size();++yaw) {
      const auto& forward=forwards[yaw];const Vec3 right(-forward.z,0,forward.x);
      const uint64_t now=1000+uint64_t(yaw)*1000;
      eye.reset();
      // A two-centimetre step on the authored segment activates the same
      // WalkingGazeDown rule used by a walking actor. Prime rays are discarded.
      casts+=eye.update({p.previous+Vec3(0,180,0),forward,right,Vec3(0,1,0),p.previous},*world.physic(),nullptr,now-500).casts;
      casts+=eye.update({p.position+Vec3(0,180,0),forward,right,Vec3(0,1,0),p.position},*world.physic(),nullptr,now).casts;
      NavigationMemory::Pose body;body.position=p.position;body.forward=forward;body.gameMs=now;
      body.stepHeightCm=step>0.f?step:50.f;body.maxSlopeDegrees=maxSlope;
      memory.update(eye.memorySamples(),body);
    }
    map->bake(memory,uint32_t(sources.size()-1));
    // Canonical-source selection is associative; compacting bounds temporary
    // observations without joining evidence cast from different poses.
    if(sources.size()%128==0) map->finish();
  }
  map->finish();sources.shrink_to_fit();
  const auto cost=std::chrono::duration<double,std::micro>(Clock::now()-start).count();
  state.familiar=std::move(map);state.familiarSources=std::move(sources);
  if(familiarBytes(300)>FamiliarMaxBytes) throw std::runtime_error("familiar atlas exceeds the 50 MB world budget for 300 NPCs");
  saveFamiliar(file);
  return "{\"kind\":\"familiar_baked\",\"file\":"+quote(file)+",\"file_bytes\":"+std::to_string(std::filesystem::file_size(file))+
      ",\"casts\":"+std::to_string(casts)+",\"candidate_poses\":"+std::to_string(poses.size())+
      ",\"accepted_poses\":"+std::to_string(state.familiarSources.size())+",\"rejected_poses\":"+std::to_string(rejected)+
      ",\"eye_layout\":250,\"eye_rays\":"+std::to_string(eye.rayCount())+",\"eye_height_cm\":180,\"bake_us\":"+number(cost)+','+familiarStats(300)+"}";
}

std::string enable(World& world,const std::string& selector,bool stranger,const std::string& origin) {
  auto& npc=resolve(world,selector);
  Actor actor;actor.npc=&npc;actor.handleId=npc.handle().id;actor.selector=selector;
  actor.spawn=origin.empty()?npc.position():goal(world,origin);actor.routines=routines(world,npc);
  const auto points=routineNodes(actor.routines);
  const auto start=Clock::now();actor.knowledge=state.graph->build(points,actor.spawn,stranger);
  const auto cost=std::chrono::duration<double,std::micro>(Clock::now()-start).count();
  const std::string result="{\"kind\":\"roads_enabled\",\"actor\":"+quote(selector)+",\"instance\":"+quote(symbol(world,npc))+
      ",\"npc_id\":"+std::to_string(world.npcId(&npc))+",\"spawn_cm\":"+vector(actor.spawn)+','+stats(actor.knowledge)+
      ",\"build_us\":"+number(cost)+",\"routines\":"+routineJson(actor.routines)+"}";
  auto it=std::find_if(state.actors.begin(),state.actors.end(),[&](const Actor& old){return old.npc==&npc;});
  if(it==state.actors.end()) state.actors.push_back(std::move(actor));else *it=std::move(actor);
  return result;
}
std::string snapshot(World& world,const std::string& tag,const std::string& selector) {
  auto& actor=tracked(world,selector);
  const auto result="{\"kind\":\"roads_snapshot\",\"tag\":"+quote(tag)+",\"actor\":"+quote(selector)+
      ",\"instance\":"+quote(symbol(world,*actor.npc))+",\"npc_id\":"+std::to_string(world.npcId(actor.npc))+
      ",\"spawn_cm\":"+vector(actor.spawn)+','+stats(actor.knowledge)+",\"routines\":"+routineJson(actor.routines)+
      ",\"known_node_ids\":"+maskJson(actor.knowledge.nodes)+",\"known_edge_ids\":"+maskJson(actor.knowledge.edges)+
      (actor.hasPath?",\"last_path\":"+pathJson(actor.lastPath,actor.knowledge):std::string{})+"}";
  append("roads-snapshots.jsonl",result);
  return "{\"kind\":\"roads_snapshot_written\",\"tag\":"+quote(tag)+",\"actor\":"+quote(selector)+','+stats(actor.knowledge)+"}";
}
std::string add(World& world,const std::string& selector,const std::string& name) {
  auto& actor=tracked(world,selector);
  const auto before=count(actor.knowledge.edges);const auto key=upper(name);
  std::vector<size_t> destinations;
  for(size_t p=0;p<state.places.size();++p) if(upper(state.places[p].id)==key) {
    for(size_t n=0;n<state.graph->nodes().size();++n) {
      const auto it=state.pointPlace.find(state.graph->nodes()[n].name);
      if(it!=state.pointPlace.end() && it->second==p) destinations.push_back(n);
    }
    if(destinations.empty()) destinations.push_back(state.graph->nearest(goal(world,name)));
    break;
  }
  if(destinations.empty()) {
    const auto node=nodeFor(world,name);
    if(node==KnownRoads::invalid) throw std::runtime_error("roads unknown place or waypoint: "+name);
    destinations.push_back(node);
  }
  const bool connected=state.graph->add(actor.knowledge,destinations);actor.hasPath=false;
  actor.destinations.push_back(goal(world,name));
  return "{\"kind\":\"roads_added\",\"actor\":"+quote(selector)+",\"destination\":"+quote(name)+
      ",\"major_connection\":"+(connected?"true":"false")+",\"added_edges\":"+std::to_string(count(actor.knowledge.edges)-before)+','+stats(actor.knowledge)+"}";
}
std::string path(World& world,const std::string& selector,const std::string& destination,const std::string& origin) {
  auto& actor=tracked(world,selector);
  const auto start=origin.empty()?actor.npc->position():goal(world,origin);
  const auto target=goal(world,destination);
  const auto begin=Clock::now();actor.lastPath=state.graph->path(actor.knowledge,start,target);actor.hasPath=true;
  const auto cost=std::chrono::duration<double,std::micro>(Clock::now()-begin).count();
  KnownRoads::Knowledge all;
  all.nodes.assign(state.graph->nodes().size(),1);all.edges.assign(state.graph->edges().size(),1);
  const auto full=state.graph->path(all,start,target);
  const auto result="{\"kind\":\"roads_path\",\"actor\":"+quote(selector)+",\"goal\":"+quote(destination)+
      ",\"start_cm\":"+vector(start)+",\"goal_cm\":"+vector(target)+",\"query_us\":"+number(cost)+
      ",\"path\":"+pathJson(actor.lastPath,actor.knowledge)+",\"full_network\":"+pathJson(full,all)+"}";
  append("roads-paths.jsonl",result);return result;
}
std::string actors(World& world,const std::string& filter) {
  std::ostringstream out;out << "{\"kind\":\"roads_actors\",\"actors\":[";bool first=true;
  for(uint32_t id=0;id<world.npcCount();++id) if(auto* npc=world.npcById(id)) {
    const auto instance=symbol(world,*npc);
    if(!filter.empty() && upper(instance).find(upper(filter))==std::string::npos) continue;
    if(!first) out << ',';
    first=false;
    out << "{\"npc_id\":" << id << ",\"instance\":" << quote(instance)
        << ",\"position_cm\":" << vector(npc->position()) << ",\"routines\":" << routineJson(routines(world,*npc)) << '}';
  }
  out << "]}";return out.str();
}
std::string distribution(std::vector<double> values) {
  if(values.empty()) return "{}";
  double sum=0;
  for(auto v:values) sum+=v;
  std::sort(values.begin(),values.end());
  return "{\"samples\":"+std::to_string(values.size())+",\"median_us\":"+number(values[values.size()/2])+
      ",\"p95_us\":"+number(values[size_t(std::ceil(double(values.size())*.95))-1])+",\"mean_us\":"+number(sum/double(values.size()))+"}";
}
std::string bench(World& world,size_t wanted,const std::string& destination,size_t repeats) {
  if(!wanted || wanted>500 || !repeats || repeats>1000) throw std::runtime_error("roads bench count 1..500, repeats 1..1000");
  struct Input {Npc* npc;std::string instance;std::vector<size_t> routines;};
  std::vector<Input> inputs;
  for(uint32_t id=0;id<world.npcCount();++id) if(auto* npc=world.npcById(id)) {
    auto nodes=routineNodes(routines(world,*npc));
    if(!nodes.empty()) inputs.push_back({npc,symbol(world,*npc),std::move(nodes)});
  }
  std::sort(inputs.begin(),inputs.end(),[](const Input& a,const Input& b){return a.instance<b.instance;});
  if(inputs.size()<wanted) throw std::runtime_error("roads bench not enough routine NPCs");
  inputs.resize(wanted);const auto target=goal(world,destination);
  std::vector<double> builds,paths;builds.reserve(wanted*repeats);paths.reserve(wanted*repeats);
  std::ostringstream timing;
  for(size_t r=0;r<repeats;++r) for(const auto& input:inputs) {
    const auto start=Clock::now();auto knowledge=state.graph->build(input.routines,input.npc->position());
    const auto built=Clock::now();const auto route=state.graph->path(knowledge,input.npc->position(),target);
    const auto done=Clock::now();
    const auto buildUs=std::chrono::duration<double,std::micro>(built-start).count();
    const auto pathUs=std::chrono::duration<double,std::micro>(done-built).count();
    builds.push_back(buildUs);paths.push_back(pathUs);
    timing << "{\"kind\":\"roads_cost\",\"instance\":" << quote(input.instance) << ",\"repeat\":" << r
           << ",\"build_us\":" << number(buildUs) << ",\"path_us\":" << number(pathUs)
           << ",\"known_nodes\":" << count(knowledge.nodes) << ",\"known_edges\":" << count(knowledge.edges)
           << ",\"path_edges\":" << route.edges.size() << "}\n";
  }
  std::ofstream raw(state.directory+"/roads-timing.jsonl",std::ios::app);
  if(!raw) throw std::runtime_error("cannot open roads timing journal");
  raw << timing.str();
  if(!raw) throw std::runtime_error("cannot write roads timing journal");
  return "{\"kind\":\"roads_bench\",\"actors\":"+std::to_string(wanted)+",\"repeats\":"+std::to_string(repeats)+
      ",\"goal\":"+quote(destination)+",\"graph_build_included\":false,\"routine_mapping_included\":false,\"build\":"+distribution(builds)+
      ",\"path\":"+distribution(paths)+"}";
}
}

void KnownRoadsBench::reset() { state=State{}; }

KnownRoads::Path KnownRoadsBench::readPath(World& world,const Npc& npc,const Vec3& goal,Vec3& next) {
  requireGraph();
  for(const auto& actor:state.actors) if(actor.npc==&npc && live(actor,world)) {
    auto result=state.graph->path(actor.knowledge,npc.position(),goal);
    next=goal;
    for(auto node:result.nodes) {
      next=state.graph->nodes()[node].position;
      if((next-npc.position()).length()>100.f) break;
    }
    return result;
  }
  throw std::runtime_error("nav demos requires roads on for the walking actor");
}

void KnownRoadsBench::readGuidance(World& world,const Npc& npc,const Vec3& position,
                                 const Vec3& goal,KnownRoads::Guidance& guidance) {
  requireGraph();
  for(const auto& actor:state.actors) if(actor.npc==&npc && live(actor,world)) {
    state.graph->updateGuidance(actor.knowledge,position,goal,guidance);
    return;
    }
  throw std::runtime_error("nav demos requires roads on for the walking actor");
}

void KnownRoadsBench::attachFamiliar(World& world,const Npc& npc,NavigationMemory& memory) {
  memory.setFamiliarMap(nullptr,{});
  if(const char* enabled=std::getenv("OPENGOTHIC_NAV_FAMILIAR_MAP");enabled && std::string_view(enabled)=="0") return;
  if(!state.familiar) return;
  requireGraph();
  for(const auto& actor:state.actors) if(actor.npc==&npc && live(actor,world)) {
    if(!actor.knowledge.stranger) memory.setFamiliarMap(state.familiar.get(),familiarMask(world,actor));
    return;
  }
  throw std::runtime_error("familiar map requires roads on for the walking actor");
}

void KnownRoadsBench::tick(World* world) {
  if(state.world==world && (!world || state.worldName==world->name())) return;
  // Loading or switching the world invalidates all NPC pointers and graph IDs.
  const auto directory=state.directory;state=State{};state.directory=directory;
  state.world=world;if(world) state.worldName=std::string(world->name());
}

std::string KnownRoadsBench::command(const std::vector<std::string>& args,World& world,const std::string& outputDirectory) {
  tick(&world);
  if(state.directory.empty()) {
    state.directory=outputDirectory;
    for(const char* file:{"roads-events.jsonl","roads-paths.jsonl","roads-snapshots.jsonl","roads-timing.jsonl"}) {
      std::ofstream out(state.directory+'/'+file,std::ios::trunc);
      if(!out) throw std::runtime_error(std::string("cannot initialize roads journal ")+file);
    }
  }
  std::string result;
  if(args.size()==3 && args[1]=="load") result=load(world,args[2]);
  else {
    requireGraph();
    if((args.size()==3 || args.size()==4) && (args[1]=="on" || args[1]=="stranger"))
      result=enable(world,args[2],args[1]=="stranger",args.size()==4?args[3]:std::string{});
    else if(args.size()==3 && args[1]=="bake") result=bakeFamiliar(world,args[2]);
    else if(args.size()==3 && args[1]=="familiar-load") result=loadFamiliar(args[2]);
    else if((args.size()==3 || args.size()==4) && args[1]=="familiar-stats") {
      const size_t npcCount=args.size()==4?std::stoull(args[3]):300;
      if(!npcCount || npcCount>500) throw std::runtime_error("familiar-stats NPC count must be 1..500");
      if(!state.familiar) throw std::runtime_error("roads bake or familiar-load is required first");
      const auto& actor=tracked(world,args[2]);std::string details;
      const auto mask=familiarMask(world,actor,&details);
      result="{\"kind\":\"familiar_stats\",\"actor\":"+quote(args[2])+",\"stranger\":"+(actor.knowledge.stranger?"true":"false")+
          ",\"actual_mask_bytes\":"+std::to_string(sizeof(mask)+mask.capacity()*sizeof(uint64_t))+','+details+','+familiarStats(npcCount)+"}";
    }
    else if(args.size()==4 && args[1]=="add") result=add(world,args[2],args[3]);
    else if((args.size()==4 || args.size()==5) && args[1]=="path") result=path(world,args[2],args[3],args.size()==5?args[4]:std::string{});
    else if(args.size()==4 && args[1]=="snapshot") result=snapshot(world,args[2],args[3]);
    else if((args.size()==2 || args.size()==3) && args[1]=="actors") result=actors(world,args.size()==3?args[2]:std::string{});
    else if((args.size()==4 || args.size()==5) && args[1]=="bench") result=bench(world,std::stoull(args[2]),args[3],args.size()==5?std::stoull(args[4]):10);
    else throw std::runtime_error("roads: load <places.json> | on|stranger <actor> [spawn] | add <actor> <place|waypoint> | path <actor> <goal> [start] | snapshot <tag> <actor> | actors [filter] | bench <count> <goal> [repeats] | bake <absolute-file> | familiar-load <absolute-file> | familiar-stats <actor> [npc-count]");
  }
  append("roads-events.jsonl",result);return result;
}

// Offline replay of familiar-road guidance (KnownRoads::updateGuidance) on
// recorded positions; no world, physics or policy. scripts/nav_roads_carrot.py
// writes the text input and compiles this file together with one revision of
// OpenGothic/common/game/knownroads.cpp, so the same trajectories are replayed
// by the old and the new rule.
//
// Usage: replay [NAVMESH [never|oracle]]. Built with -DROADS_ORACLE and Detour,
// NAVMESH (an OGNAV001 cache of the stand) answers whether a straight segment
// leaves the walkable surface: the oracle of the measurement, never of the
// game. With `oracle`, a revision that takes a passage check gets that answer
// as the walker's memory: Free where the segment stays on the navmesh, Closed
// where it hits its boundary. `never` (the default) passes no check.
//
// Input (whitespace separated):
//   graph N E, then N lines "x y z", then E lines "a b length_cm major"
//   episode ID stranger R node... spawn_x spawn_y spawn_z goal_x goal_y goal_z T,
//   then T lines "x y z"
//   segments N, then N lines "ax ay az bx by bz" (needs NAVMESH)
// Output: per episode "episode ID known_nodes known_edges", per decision
//   "d progress_cm rebuilt beyond carrot_x carrot_y carrot_z remaining_known_cm
//    caught_up start_skipped start_fallback" (the last three are 0 for a revision
//   without a passage check; start_fallback 1 is the closest node, 2 the node
//   where the walker left its previous route), after each rebuild "polyline K x y z ...", and per
//   segment "s V": 1 the segment hits the navmesh boundary and the walkable way
//   around is more than 1 m longer, 0 it does not, -1 its start is off the navmesh.
#include "game/knownroads.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef ROADS_ORACLE
#include <DetourAlloc.h>
#include <DetourNavMesh.h>
#include <DetourNavMeshQuery.h>
#endif

namespace {
#ifdef ROADS_ORACLE
template<class T> void read(std::istream& in, T& value) {
  if(!in.read(reinterpret_cast<char*>(&value),sizeof(value))) throw std::runtime_error("truncated navmesh cache");
  }

uint64_t hash(const unsigned char* data, size_t size) {
  uint64_t result = 14695981039346656037ull;
  for(size_t i=0; i<size; ++i) {result^=data[i];result*=1099511628211ull;}
  return result;
  }

// The stand's navmesh cache, as scripts/nav_memory_v4_geometry.cpp loads it.
class Oracle final {
  public:
    explicit Oracle(const std::string& path) {
      std::ifstream in(path,std::ios::binary);
      if(!in) throw std::runtime_error("cannot read navmesh cache: "+path);
      std::array<char,8> magic{};uint32_t version=0,endian=0;uint64_t key=0,count=0;double buildSeconds=0;
      dtNavMeshParams params{};
      if(!in.read(magic.data(),8) || std::memcmp(magic.data(),"OGNAV001",8)!=0) throw std::runtime_error("invalid navmesh cache magic");
      read(in,version);read(in,endian);read(in,key);read(in,buildSeconds);read(in,params);read(in,count);
      if(version!=2 || endian!=0x01020304u) throw std::runtime_error("unsupported navmesh cache format");
      if(dtStatusFailed(mesh->init(&params))) throw std::runtime_error("cannot initialize Detour mesh");
      for(uint64_t i=0; i<count; ++i) {
        dtTileRef reference=0;uint32_t size=0;uint64_t checksum=0;
        read(in,reference);read(in,size);read(in,checksum);
        if(size<sizeof(dtMeshHeader) || size>64u*1024u*1024u) throw std::runtime_error("invalid navmesh tile size");
        std::unique_ptr<unsigned char,decltype(&dtFree)> data(static_cast<unsigned char*>(dtAlloc(size,DT_ALLOC_PERM)),dtFree);
        if(!data || !in.read(reinterpret_cast<char*>(data.get()),size) || hash(data.get(),size)!=checksum)
          throw std::runtime_error("navmesh tile checksum mismatch");
        if(dtStatusFailed(mesh->addTile(data.get(),int(size),DT_TILE_FREE_DATA,reference,nullptr)))
          throw std::runtime_error("cannot add Detour tile");
        data.release();
        }
      // Native cache-v2 compatibility: bit 2 selects ordinary ground only.
      for(int i=0; i<mesh->getMaxTiles(); ++i) {
        const auto* tile = static_cast<const dtNavMesh&>(*mesh).getTile(i);
        if(!tile || !tile->header) continue;
        for(int j=0; j<tile->header->polyCount; ++j)
          if(tile->polys[j].getType()==DT_POLYTYPE_GROUND)
            mesh->setPolyFlags(mesh->getPolyRefBase(tile)|dtPolyRef(j),tile->polys[j].flags|2);
        }
      if(dtStatusFailed(query->init(mesh.get(),65535))) throw std::runtime_error("cannot initialize Detour query");
      filter.setIncludeFlags(2);
      }

    // 1: the straight segment hits the walkable boundary (wall, ledge, drop)
    // before its last 50 cm and the walkable path around is more than 1 m
    // longer, or there is none; 0: it does not; -1: its start is off the
    // navmesh. The detour keeps a ray that grazes a polygon vertex from
    // counting as a wall.
    int crosses(const Tempest::Vec3& a, const Tempest::Vec3& b) const {
      const float start[] = {a.x*.01f,a.y*.01f,a.z*.01f}, end[] = {b.x*.01f,b.y*.01f,b.z*.01f};
      const float extents[] = {.5f,2.f,.5f};
      float snapped[3]{}, target[3]{};
      dtPolyRef reference = 0, goal = 0;
      if(dtStatusFailed(query->findNearestPoly(start,extents,&filter,&reference,snapped)) || !reference)
        return -1;
      float t = 0, normal[3]{};
      std::array<dtPolyRef,256> ray{};
      int count = 0;
      if(dtStatusFailed(query->raycast(reference,snapped,end,&filter,&t,normal,ray.data(),&count,int(ray.size()))))
        return -1;
      if(t==FLT_MAX)
        return 0;
      const float length = std::hypot(end[0]-snapped[0],end[2]-snapped[2]);
      if(length*(1.f-t)<=.5f)
        return 0;
      if(dtStatusFailed(query->findNearestPoly(end,extents,&filter,&goal,target)) || !goal)
        return 1;
      corridor.resize(8192);
      const auto found = query->findPath(reference,goal,snapped,target,&filter,corridor.data(),&count,int(corridor.size()));
      if(dtStatusFailed(found) || dtStatusDetail(found,DT_PARTIAL_RESULT) || count==0 || corridor[size_t(count-1)]!=goal)
        return 1;
      corners.resize(3*1024);
      flags.resize(1024);
      int cornerCount = 0;
      if(dtStatusFailed(query->findStraightPath(snapped,target,corridor.data(),count,corners.data(),flags.data(),
                                                nullptr,&cornerCount,int(flags.size()))) || cornerCount==0)
        return 1;
      double walked = 0;
      for(int i=1; i<cornerCount; ++i) {
        const float* p = &corners[size_t(i)*3];
        walked += std::hypot(double(p[0]-p[-3]),double(p[2]-p[-1]));
        }
      return walked>double(std::hypot(target[0]-snapped[0],target[2]-snapped[2]))+1. ? 1 : 0;
      }

  private:
    std::unique_ptr<dtNavMesh,decltype(&dtFreeNavMesh)> mesh{dtAllocNavMesh(),dtFreeNavMesh};
    std::unique_ptr<dtNavMeshQuery,decltype(&dtFreeNavMeshQuery)> query{dtAllocNavMeshQuery(),dtFreeNavMeshQuery};
    dtQueryFilter filter;
    mutable std::vector<dtPolyRef> corridor;
    mutable std::vector<float> corners;
    mutable std::vector<unsigned char> flags;
  };
#else
class Oracle final {
  public:
    explicit Oracle(const std::string&) { throw std::runtime_error("built without ROADS_ORACLE"); }
    int crosses(const Tempest::Vec3&, const Tempest::Vec3&) const { return -1; }
  };
#endif

template<class Roads> concept PassageApi = requires { typename Roads::PassageCheck; };

struct Diagnosis final { int caughtUp = 0, startSkipped = 0, startFallback = 0; }; // fallback 2: departure

// One decision of either revision: the newer one takes the oracle as memory.
template<class Roads>
Diagnosis update(const Roads& roads, const typename Roads::Knowledge& knowledge, const Tempest::Vec3& position,
                 const Tempest::Vec3& goal, typename Roads::Guidance& guidance, const Oracle* memory) {
  Diagnosis result;
  if constexpr (PassageApi<Roads>) {
    typename Roads::PassageCheck check;
    if(memory!=nullptr)
      check = [memory](const Tempest::Vec3& from, const Tempest::Vec3& to) {
        const int crosses = memory->crosses(from,to);
        return crosses<0 ? Roads::Passage::Unknown : crosses ? Roads::Passage::Closed : Roads::Passage::Free;
        };
    roads.updateGuidance(knowledge,position,goal,guidance,check);
    result.caughtUp = guidance.caughtUp;
    result.startSkipped = int(guidance.route.startSkipped);
    result.startFallback = guidance.route.startFallback;
    if constexpr (requires { guidance.route.startDeparture; })
      result.startFallback += guidance.route.startDeparture;
    }
  else {
    if(memory!=nullptr)
      throw std::runtime_error("this revision takes no passage check");
    roads.updateGuidance(knowledge,position,goal,guidance);
    }
  return result;
  }
}

int main(int argc, char** argv) {
  std::ios::sync_with_stdio(false);
  std::unique_ptr<Oracle> oracle;
  if(argc>=2 && std::string(argv[1])!="-")
    oracle = std::make_unique<Oracle>(argv[1]);
  const std::string mode = argc>=3 ? argv[2] : "never";
  if(mode!="never" && mode!="oracle")
    throw std::runtime_error("passage mode must be never or oracle");
  if(mode=="oracle" && !oracle)
    throw std::runtime_error("oracle passage needs a navmesh");
  const Oracle* memory = mode=="oracle" ? oracle.get() : nullptr;
  std::unique_ptr<KnownRoads> roads;
  std::string word;
  while(std::cin >> word) {
    if(word=="graph") {
      size_t nodeCount=0, edgeCount=0;
      std::cin >> nodeCount >> edgeCount;
      std::vector<KnownRoads::Node> nodes(nodeCount);
      for(auto& node:nodes)
        std::cin >> node.position.x >> node.position.y >> node.position.z;
      std::vector<KnownRoads::Edge> edges(edgeCount);
      for(auto& edge:edges) {
        int major=0;
        std::cin >> edge.a >> edge.b >> edge.lengthCm >> major;
        edge.major = major!=0;
        }
      if(!std::cin)
        throw std::runtime_error("truncated graph");
      roads = std::make_unique<KnownRoads>(std::move(nodes),std::move(edges));
      continue;
      }
    if(word=="segments") {
      if(!oracle)
        throw std::runtime_error("segments need a navmesh");
      size_t count=0;
      std::cin >> count;
      for(size_t i=0; i<count; ++i) {
        Tempest::Vec3 a, b;
        std::cin >> a.x >> a.y >> a.z >> b.x >> b.y >> b.z;
        if(!std::cin)
          throw std::runtime_error("truncated segments");
        std::printf("s %d\n",oracle->crosses(a,b));
        }
      continue;
      }
    if(word!="episode" || !roads)
      throw std::runtime_error("expected graph before episode, got "+word);
    std::string id;
    int stranger=0;
    size_t routineCount=0;
    std::cin >> id >> stranger >> routineCount;
    std::vector<size_t> routine(routineCount);
    for(auto& node:routine)
      std::cin >> node;
    Tempest::Vec3 spawn, goal;
    size_t decisions=0;
    std::cin >> spawn.x >> spawn.y >> spawn.z >> goal.x >> goal.y >> goal.z >> decisions;
    if(!std::cin)
      throw std::runtime_error("truncated episode header "+id);
    const auto knowledge = roads->build(routine,spawn,stranger!=0);
    size_t knownNodes=0, knownEdges=0;
    for(auto bit:knowledge.nodes) knownNodes += bit!=0;
    for(auto bit:knowledge.edges) knownEdges += bit!=0;
    std::printf("episode %s %zu %zu\n",id.c_str(),knownNodes,knownEdges);
    KnownRoads::Guidance guidance;
    for(size_t i=0; i<decisions; ++i) {
      Tempest::Vec3 position;
      std::cin >> position.x >> position.y >> position.z;
      if(!std::cin)
        throw std::runtime_error("truncated trajectory "+id);
      const auto diagnosis = update(*roads,knowledge,position,goal,guidance,memory);
      std::printf("d %.4f %d %d %.4f %.4f %.4f %.4f %d %d %d\n",guidance.progressCm,int(guidance.rebuilt),
                  int(guidance.beyondKnown),guidance.carrot.x,guidance.carrot.y,guidance.carrot.z,
                  guidance.remainingKnownCm,diagnosis.caughtUp,diagnosis.startSkipped,diagnosis.startFallback);
      if(guidance.rebuilt) {
        std::printf("polyline %zu",guidance.polyline.size());
        for(const auto& point:guidance.polyline)
          std::printf(" %.4f %.4f %.4f",point.x,point.y,point.z);
        std::printf("\n");
        }
      }
    }
  return 0;
  }

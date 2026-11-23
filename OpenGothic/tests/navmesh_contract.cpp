#include "physics/navmesh.h"

#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
using Point = NavMesh::Point;
void check(bool condition, const char* message) {
  if(!condition) throw std::runtime_error(message);
  }
void triangle(std::vector<float>& vertices, std::vector<int>& indices, Point a, Point b, Point c) {
  const int base=int(vertices.size()/3);
  for(const auto& p:{a,b,c}) vertices.insert(vertices.end(),p.begin(),p.end());
  indices.insert(indices.end(),{base,base+1,base+2});
  }
void floor(NavMesh::Geometry& geometry, float x0, float z0, float x1, float z1, float y=0.f, bool water=false) {
  auto& vertices=water ? geometry.waterVertices : geometry.vertices;
  auto& indices=water ? geometry.waterTriangles : geometry.triangles;
  triangle(vertices,indices,{x0,y,z0},{x0,y,z1},{x1,y,z1});
  triangle(vertices,indices,{x0,y,z0},{x1,y,z1},{x1,y,z0});
  }
void wall(NavMesh::Geometry& geometry) {
  floor(geometry,9.5f,4.f,10.5f,16.f,3.f);
  auto& v=geometry.vertices;
  auto& t=geometry.triangles;
  triangle(v,t,{9.5f,0,4},{9.5f,3,4},{9.5f,3,16});
  triangle(v,t,{9.5f,0,4},{9.5f,3,16},{9.5f,0,16});
  triangle(v,t,{10.5f,0,4},{10.5f,0,16},{10.5f,3,16});
  triangle(v,t,{10.5f,0,4},{10.5f,3,16},{10.5f,3,4});
  triangle(v,t,{9.5f,0,4},{10.5f,0,4},{10.5f,3,4});
  triangle(v,t,{9.5f,0,4},{10.5f,3,4},{9.5f,3,4});
  triangle(v,t,{9.5f,0,16},{9.5f,3,16},{10.5f,3,16});
  triangle(v,t,{9.5f,0,16},{10.5f,3,16},{10.5f,0,16});
  }

void nearbyGroundContract(const std::filesystem::path& cache) {
  NavMesh mesh;
  check(mesh.nearbyGroundLocations({0,50,0}).empty(),"unbuilt mesh must not invent observer placements");
  NavMesh::Params params;params.tileSize=128;
  NavMesh::Geometry geometry;
  floor(geometry,-10,-10,10,10,50.f);
  floor(geometry,3,-4,9,4,54.f);
  mesh.buildOrLoad(geometry,params,cache,"nearby-ground");
  const Point at={0,50,0};
  const auto anchor=mesh.groundLocation(at);
  const auto candidates=mesh.nearbyGroundLocations(at);
  check(anchor.known && !candidates.empty() && candidates.size()<=80,
        "a polygon containing the actor must yield bounded interior annulus placements");
  check(candidates.front().component==anchor.component && candidates.front().snapDistance<2.1f,
        "nearby placements must prefer the source ground component and nearest distance");
  bool interior=false;
  for(size_t i=0;i<candidates.size();++i) {
    const auto& candidate=candidates[i];
    check(candidate.known && candidate.horizontalDistance>=2.f && candidate.horizontalDistance<=6.f,
          "every observer placement must remain in the inclusive 2-6m horizontal annulus");
    check(candidate.component==anchor.component && candidate.snapped[1]>49.9f && candidate.snapped[1]<50.3f,
          "a nearby balcony must not displace available ground on the actor's component");
    if(i)check(candidate.snapDistance>=candidates[i-1].snapDistance,"same-component placements must be nearest first");
    interior=interior || candidate.polygon==anchor.polygon;
  }
  check(interior,"annulus placement must sample interiors instead of only polygon closest points to the actor");
  const auto repeated=mesh.nearbyGroundLocations(at);
  check(repeated.size()==candidates.size(),"observer placement count must be repeatable");
  for(size_t i=0;i<candidates.size();++i)
    check(candidates[i].snapped==repeated[i].snapped && candidates[i].polygon==repeated[i].polygon,
          "observer placement coordinates and ordering must be deterministic without RNG");
  const auto above=mesh.nearbyGroundLocations({0,60,0});
  check(!above.empty(),"observer placement must search native ground Y beyond the old 2m vertical projection extent");
  for(const auto& candidate:above)
    check(candidate.snapped[1]>49.9f && candidate.snapped[1]<54.3f,
          "observer placement must return actual native ground height, never actor-relative guessed Y");
  check(mesh.nearbyGroundLocations({100,60,100}).empty(),"missing local ground must reject placement instead of snapping across the world");
  std::cout<<"nearby-ground contract: interior annulus, native height, component preference, deterministic ordering passed; candidates="
           <<candidates.size()<<" elevated_pose_candidates="<<above.size()<<'\n';
}
}

int main(int argc,char** argv) {
  const auto cache=std::filesystem::temp_directory_path()/
                   ("gothic-navmesh-contract-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  try {
    nearbyGroundContract(cache);
    if(argc==2 && std::string(argv[1])=="--nearby-ground") {
      std::filesystem::remove_all(cache);
      return 0;
    }
    NavMesh::Params params;
    params.tileSize=32;
    NavMesh::Geometry geometry;
    floor(geometry,0,0,20,20);
    floor(geometry,30,0,40,10);
    wall(geometry);
    NavMesh mesh;
    check(!mesh.straightWalkable({3,0,10},{17,0,10}),"unbuilt mesh must not authorize direct walking");
    check(!mesh.groundLocation({3,0,10}).known,"unbuilt mesh must not invent ground topology");
    mesh.buildOrLoad(geometry,params,cache,"contract");
    check(mesh.stats().complete && !mesh.stats().fromCache,"first build must build complete mesh");
    const auto detour=mesh.findPath({3,0,10},{17,0,10});
    check(detour.complete && detour.corners.size()>=4 && detour.length>18.f,"wall must require a real multi-tile detour");
    check(!mesh.straightWalkable({3,0,10},{17,0,10}),"wall must block a direct ground ray despite a complete detour");
    check(mesh.straightWalkable({3,0,2},{17,0,2}),"clear multi-tile ground must permit direct steering");
    check(!mesh.straightWalkable({3,0,10},{35,0,5}),"disconnected ground must not authorize direct walking");
    check(!mesh.straightWalkable({100,0,100},{17,0,10}),"off-mesh endpoint must not authorize direct walking");
    check(!mesh.findPath({3,0,10},{35,0,5}).complete,"disconnected island must reject partial paths");
    check(!mesh.findPath({100,0,100},{17,0,10}).complete,"off-mesh start must reject path");
    check(mesh.stats().cacheBytes>0 && mesh.stats().tiles>1,"tiled cache must be populated");
    const auto sameComponentSides=mesh.transitionSides({9,0,10},{11,0,10},{9,0,10});
    check(std::string(sameComponentSides.method)=="polygon" &&
          sameComponentSides.source.component==sameComponentSides.end.component &&
          sameComponentSides.source.polygon!=sameComponentSides.end.polygon &&
          sameComponentSides.takeoffSide && !sameComponentSides.landingSide,
          "connected ground around a wall must distinguish banks by endpoint polygon");
    const auto wrongPolygon=mesh.transitionSides({9,0,10},{11,0,10},{3,0,10});
    check(wrongPolygon.current.known && wrongPolygon.current.component==wrongPolygon.end.component &&
          wrongPolygon.current.polygon!=wrongPolygon.end.polygon && !wrongPolygon.landingSide,
          "being in the landing ground component must not identify the landing side within one component");
    const auto rightPolygon=mesh.transitionSides({9,0,10},{11,0,10},{11,0,10});
    check(rightPolygon.landingSide && !rightPolygon.takeoffSide,"same-component landing requires the exact end polygon");
    const auto ambiguous=mesh.transitionSides({3,0,2},{3,0,2},{3,0,2});
    check(std::string(ambiguous.method)=="ambiguous" && !ambiguous.takeoffSide && !ambiguous.landingSide,
          "coincident endpoint polygons must remain explicitly ambiguous");
    const auto shortcuts=mesh.shortcutCandidates(128);
    check(!shortcuts.candidates.empty(),"whole-world boundaries must expose shortcut probe sites");
    bool connectedShortcut=false;
    for(const auto& candidate:shortcuts.candidates) {
      const float direct=std::sqrt(std::pow(candidate.start[0]-candidate.end[0],2.f)+
                                   std::pow(candidate.start[1]-candidate.end[1],2.f)+
                                   std::pow(candidate.start[2]-candidate.end[2],2.f));
      check(candidate.horizontalDistance<=4.f && candidate.verticalDistance<=3.f,
            "shortcut endpoints must respect physical probe bounds after insetting");
      if(candidate.groundComplete) {
        connectedShortcut=true;
        check(candidate.groundDistance>=3.f*direct && candidate.groundDistance>=direct+10.f,
              "connected shortcut must save at least 10m and two thirds of ground distance");
        }
      }
    check(connectedShortcut,"same connected ground component around a wall must produce a real shortcut");
    check(shortcuts.candidates.size()<=128 && shortcuts.groundQueries<=shortcuts.queryBudget && shortcuts.queryBudget==1280,
          "shortcut sites and ground-query work must obey explicit budgets");
    const auto repeatedShortcuts=mesh.shortcutCandidates(128);
    check(repeatedShortcuts.candidates.size()==shortcuts.candidates.size() &&
          repeatedShortcuts.groundQueries==shortcuts.groundQueries,"shortcut search must be repeatable");
    for(size_t i=0; i<shortcuts.candidates.size(); ++i)
      check(shortcuts.candidates[i].start==repeatedShortcuts.candidates[i].start &&
            shortcuts.candidates[i].end==repeatedShortcuts.candidates[i].end,
            "shortcut budget ordering must be deterministic");
    NavMesh loaded;
    loaded.buildOrLoad(geometry,params,cache,"contract");
    check(loaded.stats().fromCache,"second build must load cache");
    check(loaded.findPath({3,0,10},{17,0,10}).complete,"cached mesh must retain tile connectivity");
    check(loaded.stats().key==mesh.stats().key,"cache key must be stable");
    auto changed=params;
    changed.radius+=0.1f;
    NavMesh resized;
    resized.buildOrLoad(geometry,changed,cache,"contract");
    check(!resized.stats().fromCache && resized.stats().key!=mesh.stats().key,"agent dimensions must invalidate cache");
    auto modified=geometry;
    modified.vertices[0]+=0.1f;
    NavMesh edited;
    edited.buildOrLoad(modified,params,cache,"contract");
    check(!edited.stats().fromCache && edited.stats().key!=mesh.stats().key,"collision world must invalidate cache");
    // Detect corruption before giving native Detour a cached tile's bytes.
    {
      std::fstream file(mesh.stats().cacheFile,std::ios::binary|std::ios::in|std::ios::out);
      file.seekp(-1,std::ios::end);
      file.put(char(0x7f));
    }
    NavMesh repaired;
    repaired.buildOrLoad(geometry,params,cache,"contract");
    check(!repaired.stats().fromCache && repaired.findPath({3,0,10},{17,0,10}).complete,"corrupt cache must rebuild safely");
    NavMesh::Geometry basin;
    floor(basin,0,0,20,20);
    floor(basin,8,0,12,20,2.f,true);
    NavMesh flooded;
    flooded.buildOrLoad(basin,params,cache,"water");
    check(!flooded.findPath({3,0,10},{17,0,10}).complete,"deep water must block bottom routes");
    for(size_t i=1; i<basin.waterVertices.size(); i+=3) basin.waterVertices[i]=0.25f;
    NavMesh shallow;
    shallow.buildOrLoad(basin,params,cache,"shallow");
    check(shallow.findPath({3,0,10},{17,0,10}).complete,"shallow water must retain walkable ground");
    for(size_t i=1; i<basin.waterVertices.size(); i+=3) basin.waterVertices[i]=2.f;
    // A cave ceiling occludes water above it, matching the physics water ray.
    floor(basin,0,0,20,20,1.5f);
    NavMesh cave;
    cave.buildOrLoad(basin,params,cache,"cave");
    check(cave.findPath({3,0,10},{17,0,10}).complete,"water above a solid ceiling must not flood cave floor");
    NavMesh::Geometry floors;
    floor(floors,0,0,20,20);
    floor(floors,8,0,18,20,3.f);
    NavMesh layered;
    layered.buildOrLoad(floors,params,cache,"floors");
    check(layered.straightWalkable({3,0,10},{12,0,10}),"direct walking must remain on the ground under a balcony");
    check(!layered.straightWalkable({3,0,10},{12,3,10}),"2D ray success must not confuse a balcony with the ground below");
    const auto lowerLevel=layered.transitionSides({12,0,10},{12,3,10},{12,0,10});
    const auto upperLevel=layered.transitionSides({12,0,10},{12,3,10},{12,3,10});
    check(std::string(lowerLevel.method)=="component" && lowerLevel.source.component!=lowerLevel.end.component &&
          lowerLevel.takeoffSide && !lowerLevel.landingSide && upperLevel.landingSide && !upperLevel.takeoffSide,
          "3D ground projection must distinguish stacked floors at identical XZ");
    const auto roofEdge=layered.transitionSides({12,0,10},{12,3,10},{18.1f,3.f,10.f});
    check(roofEdge.current.known && roofEdge.current.horizontalDistance>.25f && roofEdge.landingSide,
          "a physical pose outside the eroded roof edge must retain nearest-ground topology without a snap gate");
    NavMesh::Geometry gap;
    floor(gap,0,0,8,8);
    floor(gap,10,0,18,8);
    NavMesh separated;
    separated.buildOrLoad(gap,params,cache,"gap");
    const Point takeoff={3,0,4}, landing={15,0,4};
    check(!separated.findPath(takeoff,landing).complete,"unproven gap must stay disconnected");
    const auto candidates=separated.gapCandidates(takeoff,landing);
    check(!candidates.empty() && candidates.front().horizontalDistance<=4.f,"disconnected components must expose nearby boundary probe sites");
    const auto prepared=separated.takeoffSite(candidates.front().start);
    check(prepared.valid && prepared.sourcePolygon!=0 && prepared.insetDistance>0.f && prepared.insetDistance<=.1501f,
          "boundary takeoff must inset by a bounded positive amount inside its source ground polygon");
    const bool insetReachable=separated.findPath(takeoff,prepared.inset,false).complete;
    const bool insetCrossesGap=separated.findPath(prepared.inset,landing,false).complete;
    if(prepared.insetWallDistance<=prepared.sourceWallDistance+.05f || !insetReachable || insetCrossesGap) {
      const auto& source=candidates.front().start;
      std::cerr<<"takeoff fixture source=["<<source[0]<<','<<source[1]<<','<<source[2]<<"] inset=["
               <<prepared.inset[0]<<','<<prepared.inset[1]<<','<<prepared.inset[2]<<"] source_wall_m="
               <<prepared.sourceWallDistance<<" inset_wall_m="<<prepared.insetWallDistance
               <<" inset_m="<<prepared.insetDistance<<" original_island="<<insetReachable
               <<" crosses_gap="<<insetCrossesGap<<'\n';
      }
    check(prepared.insetWallDistance>prepared.sourceWallDistance+.05f,
          "bounded takeoff inset must gain more than 5cm of actual ground-wall clearance");
    check(insetReachable && !insetCrossesGap,"takeoff inset must remain on the original island without crossing the gap");
    check(!separated.takeoffSite({9.f,0.f,4.f}).valid,
          "takeoff preparation must reject a source in a gap instead of snapping across it");
    const auto balconyTakeoff=layered.takeoffSite({12.f,3.f,10.f});
    check(balconyTakeoff.valid && balconyTakeoff.inset[1]>2.75f,
          "bounded takeoff query must retain the requested balcony rather than the ground below");
    check(!layered.takeoffSite({12.f,1.5f,10.f}).valid,
          "bounded takeoff query must reject a source between storeys rather than finding another floor");
    check(separated.gapCandidates(takeoff,landing,1.f).empty(),"boundary search must respect jump distance bound");
    check(separated.gapCandidates(takeoff,{4,0,4}).empty(),"connected ground must not produce gap candidates");
    auto linkedParams=params;
    NavMesh::OffMeshLink link;
    link.start=candidates.front().start;
    link.end=candidates.front().end;
    link.type=NavMesh::LinkType::Jump;
    link.id=42;
    linkedParams.links.push_back(link);
    NavMesh linked;
    linked.buildOrLoad(gap,linkedParams,cache,"gap");
    check(linked.stats().key!=separated.stats().key,"proven transition must invalidate mesh cache");
    const auto crossed=linked.findPath(takeoff,landing);
    check(crossed.complete && crossed.transitions.size()==1,"proven jump must connect islands");
    check(!linked.straightWalkable(takeoff,landing),"an off-mesh jump must never authorize direct ground steering");
    check(!linked.findPath(takeoff,landing,false).complete,
          "ground-only baseline must remain disconnected even when a proven jump joins the islands");
    const auto sourceBank=linked.transitionSides(link.start,link.end,takeoff);
    const auto endBank=linked.transitionSides(link.start,link.end,landing);
    check(std::string(sourceBank.method)=="component" && sourceBank.source.component!=sourceBank.end.component &&
          sourceBank.takeoffSide && !sourceBank.landingSide && endBank.landingSide && !endBank.takeoffSide,
          "a proven off-mesh jump must not merge ground components or confuse the takeoff and landing banks");
    const auto& transition=crossed.transitions.front();
    check(transition.type==NavMesh::LinkType::Jump && transition.id==42,"path must retain transition kind and identity");
    check(transition.cornerIndex+1<crossed.corners.size() &&
          crossed.corners[transition.cornerIndex]==transition.start &&
          crossed.corners[transition.cornerIndex+1]==transition.end,"path must expose both directed transition endpoints as corners");
    check(!linked.findPath(landing,takeoff).complete,"one successful probe must not authorize reverse traversal");
    NavMesh linkedLoaded;
    linkedLoaded.buildOrLoad(gap,linkedParams,cache,"gap");
    const auto cachedCrossing=linkedLoaded.findPath(takeoff,landing);
    check(linkedLoaded.stats().fromCache && cachedCrossing.complete && cachedCrossing.transitions.size()==1 &&
          cachedCrossing.transitions.front().type==NavMesh::LinkType::Jump &&
          cachedCrossing.transitions.front().id==42,"cached transition must retain its connectivity, type and identity");
    check(!linkedLoaded.findPath(takeoff,landing,false).complete,
          "ground-only baseline must exclude off-mesh links from preexisting caches too");
    const auto cachedBank=linkedLoaded.transitionSides(link.start,link.end,landing);
    check(cachedBank.source.component==endBank.source.component && cachedBank.end.component==endBank.end.component &&
          cachedBank.current.polygon==endBank.current.polygon && cachedBank.landingSide,
          "ground component and polygon identifiers must be deterministic after cache load");
    NavMesh reusedTopology;reusedTopology.buildOrLoad(gap,linkedParams,cache,"gap");
    NavMesh::Geometry joinedGround;floor(joinedGround,0,0,18,8);
    reusedTopology.buildOrLoad(joinedGround,params,cache,"gap");
    const auto joinedSource=reusedTopology.groundLocation(takeoff),joinedEnd=reusedTopology.groundLocation(landing);
    check(joinedSource.known && joinedEnd.known && joinedSource.component==joinedEnd.component,
          "rebuilding a NavMesh must invalidate its previous disconnected component table");
    NavMesh::Geometry parallelBanks;floor(parallelBanks,0,0,8,8);floor(parallelBanks,8.4f,0,16.4f,8);
    NavMesh parallel;parallel.buildOrLoad(parallelBanks,params,cache,"parallel-banks");
    const auto wrongBank=parallel.transitionSides({7.8f,0,4},{8.6f,0,4},{8.05f,0,4});
    check(std::string(wrongBank.method)=="component" && wrongBank.takeoffSide && !wrongBank.landingSide &&
          wrongBank.current.component!=wrongBank.end.component,
          "a pose within 60cm of the landing endpoint on a parallel wrong bank must remain takeoff-side");
    linkedParams.links.front().type=NavMesh::LinkType::Climb;
    linkedParams.links.front().bidirectional=true;
    NavMesh climb;
    climb.buildOrLoad(gap,linkedParams,cache,"gap");
    const auto reverseClimb=climb.findPath(landing,takeoff);
    check(climb.stats().key!=linked.stats().key && reverseClimb.complete &&
          reverseClimb.transitions.size()==1 && reverseClimb.transitions.front().type==NavMesh::LinkType::Climb,
          "explicitly verified reverse climb must be typed and cached separately");
    linkedParams.links.front().type=NavMesh::LinkType::Drop;
    NavMesh drop;
    drop.buildOrLoad(gap,linkedParams,cache,"gap");
    const auto dropped=drop.findPath(takeoff,landing);
    check(drop.stats().key!=climb.stats().key && dropped.complete &&
          dropped.transitions.front().type==NavMesh::LinkType::Drop,"drop kind must survive mesh construction and change cache key");
    mesh.writePolygons(cache/"polygons.jsonl");
    check(std::filesystem::file_size(cache/"polygons.jsonl")>0,"polygon plot export must be nonempty");
    std::filesystem::remove_all(cache);
    std::cout << "navmesh contract: detour, direct-ground rays, layered-floor rejection, disconnected/off-mesh rejection, tiled cache, invalidation, corruption, deep/shallow water, cave, gap boundaries, directed jump/climb/drop links, export passed\n";
    return 0;
  } catch(const std::exception& error) {
    std::filesystem::remove_all(cache);
    std::cerr << "navmesh contract: " << error.what() << '\n';
    return 1;
    }
  }

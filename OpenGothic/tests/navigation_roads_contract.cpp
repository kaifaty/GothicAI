#include "game/knownroads.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
using Roads = KnownRoads;
using Tempest::Vec3;

void require(bool condition, const char* message) {
  if(condition)
    return;
  std::fprintf(stderr,"FAIL: %s\n",message);
  std::exit(EXIT_FAILURE);
  }

void close(double actual, double expected, const char* message) {
  require(std::isfinite(actual) && std::abs(actual-expected)<.01,message);
  }

std::vector<Roads::Node> nodes() {
  return {{"gate",{0,0,0}}, {"street1",{10000,0,0}}, {"street2",{20000,0,0}},
          {"street3",{30000,0,0}}, {"street4",{40000,0,0}}, {"home",{20000,0,3000}},
          {"alley1",{15000,0,3000}}, {"alley2",{5000,0,3000}}, {"courtyard",{15000,0,3500}},
          {"near_but_detour",{20001,0,3000}}, {"island1",{100,0,50}}, {"island2",{200,0,50}}};
  }

std::vector<Roads::Edge> edges() {
  return {{0,1,10000,true}, {1,2,10000,true}, {2,3,10000,true}, {3,4,10000,true},
          {2,5,3000,false}, {5,6,5000,false}, {6,7,10000,false}, {7,0,6000,false},
          {6,8,500,false}, {5,9,4100,false}, {10,11,100,false}};
  }

void familiarEdgesOnly(const Roads& roads, const Roads::Knowledge& knowledge, const Roads::Path& path) {
  require(path.nodes.size()==path.edges.size()+1,"nonempty path has one more node than edges");
  double length = 0;
  for(size_t i=0; i<path.edges.size(); ++i) {
    const size_t id = path.edges[i];
    require(id<knowledge.edges.size() && knowledge.edges[id],"every returned edge is familiar");
    const auto& edge = roads.edges()[id];
    require((edge.a==path.nodes[i] && edge.b==path.nodes[i+1]) ||
            (edge.b==path.nodes[i] && edge.a==path.nodes[i+1]),"edge joins the returned consecutive nodes");
    require(knowledge.nodes[path.nodes[i]] && knowledge.nodes[path.nodes[i+1]],"returned nodes are familiar");
    length += edge.lengthCm;
    }
  close(path.lengthCm,length,"reported length is sum of returned edges");
  }

void localAndStranger() {
  const Roads roads(nodes(),edges());
  const Vec3 start = roads.nodes()[0].position;
  const Vec3 goal = roads.nodes()[5].position;
  const auto local = roads.build({0,5},start);
  const auto stranger = roads.build({},start);
  require(!local.stranger && stranger.stranger,"routine distinguishes local from stranger");
  require(local.nodes[2] && local.edges[4],"30 m routine neighbourhood reaches road by network distance");
  require(!local.nodes[9] && !local.edges[9],"41 m path stays outside 40 m despite 1 cm straight separation");
  require(local.edges[5] && local.edges[6] && local.edges[7],"daily route teaches the shorter alley");
  require(stranger.nodes[3] && stranger.edges[2],"major road exactly 300 m away is familiar");
  require(!stranger.nodes[4] && !stranger.edges[3],"major road beyond 300 m is unknown");
  for(size_t i=0; i<roads.edges().size(); ++i)
    require(!stranger.edges[i] || roads.edges()[i].major,"stranger knows only major edges");

  const auto a = roads.path(local,start,goal);
  const auto b = roads.path(stranger,start,goal);
  familiarEdgesOnly(roads,local,a);
  familiarEdgesOnly(roads,stranger,b);
  require(a.goalInKnown && !b.goalInKnown,"goal matches only local's reachable familiar node");
  require(a.edges==std::vector<size_t>({7,6,5}),"local cuts through the daily alley");
  close(a.lengthCm,21000,"local path length");
  close(a.remainingCm,0,"local reaches goal");
  close(b.lengthCm,20000,"stranger follows main street");
  close(b.remainingCm,3000,"stranger leaves 30 m to the eyes");
  close(b.direction.x,0,"remainder x direction");
  close(b.direction.z,1,"remainder z direction");
  const auto forced = roads.build({0,5},start,true);
  require(forced.nodes==stranger.nodes && forced.edges==stranger.edges,"explicit stranger ignores local routine");
  std::puts("local/stranger: 210+0 m alley versus 200+30 m main street; 40/300 m network boundaries");
  }

void unknownGoalAndAdd() {
  const Roads roads(nodes(),edges());
  const Vec3 start = roads.nodes()[0].position;
  auto local = roads.build({0,5},start);
  const auto frontier = roads.path(local,start,roads.nodes()[8].position);
  familiarEdgesOnly(roads,local,frontier);
  require(frontier.nodes.back()==6 && !frontier.goalInKnown,"unknown courtyard ends at closest familiar node");
  close(frontier.remainingCm,500,"unknown goal leaves five metres");

  auto stranger = roads.build({},start);
  require(roads.add(stranger,{5}),"added home connects to existing major road");
  const auto home = roads.path(stranger,start,roads.nodes()[5].position);
  familiarEdgesOnly(roads,stranger,home);
  close(home.lengthCm,23000,"added place uses main road and its local approach");
  close(home.remainingCm,0,"addition removes the 30 m remainder");
  require(home.goalInKnown && !stranger.edges[5],"addition does not teach a remote alley shortcut");
  require(roads.add(stranger,{4}),"added distant street learns major connector beyond original range");
  require(stranger.edges[3],"added route really grows major road knowledge");
  const auto far = roads.path(stranger,start,roads.nodes()[4].position);
  familiarEdgesOnly(roads,stranger,far);
  close(far.lengthCm,40000,"added distant street route length");
  require(!roads.add(local,{10}),"disconnected place has no major connector");
  const auto island = roads.path(local,start,roads.nodes()[10].position);
  familiarEdgesOnly(roads,local,island);
  require(local.nodes[10] && local.nodes[11],"disconnected destination neighbourhood is remembered");
  require(island.nodes.back()==0 && island.goalInKnown,"known disconnected island stays familiar but query cannot jump to it");
  std::puts("frontier/add: unknown remainder 5 m; added home 230+0 m; distant major connector 400 m; islands stay unreachable");
  }

void acquisitionAndDeterminism() {
  const Roads cycle({{"a",{0,0,0}},{"b",{10000,0,0}},{"c",{5000,0,8000}}},
                    {{0,1,10000,false},{1,2,10000,false},{2,0,10000,false}});
  const auto daily = cycle.build({0,1,2},{0,0,0});
  require(std::all_of(daily.edges.begin(),daily.edges.end(),[](uint8_t v){return v!=0;}),
          "daily routine includes last-to-first trip");

  const Roads roads(nodes(),edges());
  const auto reference = roads.build({0,5},{0,0,0});
  const auto route = roads.path(reference,{0,0,0},roads.nodes()[8].position);
  for(int i=0; i<16; ++i) {
    const auto rebuilt = roads.build({0,5},{0,0,0});
    require(rebuilt.nodes==reference.nodes && rebuilt.edges==reference.edges,"knowledge rebuild is deterministic");
    const auto repeated = roads.path(rebuilt,{0,0,0},roads.nodes()[8].position);
    require(repeated.nodes==route.nodes && repeated.edges==route.edges,"path is deterministic");
    close(repeated.lengthCm,route.lengthCm,"deterministic route length");
    }

  auto alteredNodes = nodes();
  auto alteredEdges = edges();
  alteredNodes[8].position = {0,0,1};
  alteredNodes[9].position = roads.nodes()[8].position;
  alteredEdges[8] = {0,5,1,false}; // unknown edge would become an enormous shortcut
  alteredEdges[9] = {0,9,1,false};
  const Roads altered(std::move(alteredNodes),std::move(alteredEdges));
  const auto blind = altered.path(reference,{0,0,0},roads.nodes()[8].position);
  require(blind.nodes==route.nodes && blind.edges==route.edges && blind.goalInKnown==route.goalInKnown,
          "changing unknown coordinates/topology cannot influence a familiar-road query");
  close(blind.lengthCm,route.lengthCm,"unknown edge length cannot affect route length");
  close(blind.remainingCm,route.remainingCm,"unknown coordinates cannot affect remainder");
  close(blind.direction.x,route.direction.x,"unknown coordinates cannot affect direction x");
  close(blind.direction.z,route.direction.z,"unknown coordinates cannot affect direction z");
  const Roads empty({},{});
  const auto unknown = empty.path(empty.build({},{0,0,0}),{0,0,0},{300,0,400});
  require(unknown.nodes.empty() && unknown.edges.empty() && !unknown.goalInKnown,"empty knowledge has no invented route");
  close(unknown.remainingCm,500,"empty knowledge leaves entire straight remainder");
  std::puts("daily cycle/determinism: 16 rebuilds identical; unknown topology/coordinates do not influence query; empty graph handled");
  }

void roadClassificationSeam() {
  const Roads roads({{"a",{0,0,0}},{"b",{10000,0,0}},
                     {"c",{11000,0,0}},{"d",{21000,0,0}}},
                    {{0,1,10000,true},{1,2,1000,false},{2,3,10000,true}});
  const auto stranger = roads.build({},{0,0,0});
  require(stranger.edges[0] && stranger.edges[2],"300 m network radius includes road across a classification seam");
  require(!stranger.edges[1],"knowing a nearby road does not teach the non-major seam");
  const auto path = roads.path(stranger,{0,0,0},{21000,0,0});
  familiarEdgesOnly(roads,stranger,path);
  require(path.nodes.back()==1 && path.goalInKnown,"familiar goal across unknown classification seam stays unreachable");
  close(path.remainingCm,11000,"classification seam leaves correct remainder");
  std::puts("road classification: nearby major fragments known; unknown minor seam remains uncrossable");
  }

void forwardGuidance() {
  const Roads roads({{"a",{0,0,0}},{"b",{2000,0,0}},{"c",{4000,0,0}}},
                    {{0,1,2000,true},{1,2,2000,true}});
  const Roads::Knowledge knowledge{{1,1,1},{1,1},false};
  Roads::Guidance guidance;
  const Vec3 goal{5000,0,0};
  roads.updateGuidance(knowledge,{500,0,0},goal,guidance);
  require(guidance.rebuilt && !guidance.beyondKnown,"initial guidance builds one familiar route");
  close(guidance.progressCm,500,"initial progress is the projection on the familiar path");
  close(guidance.carrot.x,1100,"initial carrot is ahead of projection, not behind at closest node");
  close(guidance.remainingKnownCm,3500,"known remainder excludes already passed prefix");
  require(guidance.route.nodes==std::vector<size_t>({0,1,2}),"guidance preserves original familiar node IDs");

  // Real decisions move at most 0.5 m; walking passes every 6.5 m window.
  roads.updateGuidance(knowledge,{800,0,0},goal,guidance);
  roads.updateGuidance(knowledge,{1200,0,0},goal,guidance);
  require(!guidance.rebuilt,"following a route does not rebuild it");
  close(guidance.progressCm,1200,"projection advances along fixed route");
  close(guidance.carrot.x,1800,"carrot remains six metres ahead");
  roads.updateGuidance(knowledge,{900,0,0},goal,guidance);
  close(guidance.progressCm,1200,"backward movement cannot decrease projection progress");
  close(guidance.carrot.x,1800,"backward movement cannot move carrot backwards");

  for(const float x:{1800.f,2400.f,3000.f}) {
    roads.updateGuidance(knowledge,{x,0,0},goal,guidance);
    require(!guidance.beyondKnown,"familiar road remains ahead");
    }
  roads.updateGuidance(knowledge,{3500,0,0},goal,guidance);
  require(guidance.beyondKnown && !guidance.rebuilt,"lookahead passing familiar endpoint hands off to eyes");
  close(guidance.carrot.x,goal.x,"carrot after final familiar node is actual goal");
  roads.updateGuidance(knowledge,{4600,0,0},goal,guidance);
  close(guidance.remainingKnownCm,0,"known remainder reaches zero beyond endpoint");
  roads.updateGuidance(knowledge,{5000,0,0},goal,guidance);
  require(guidance.beyondKnown && !guidance.rebuilt,"goal approach never rebuilds back to familiar endpoint");
  close(guidance.carrot.x,goal.x,"terminal carrot remains the goal");
  std::puts("guidance: initial projection passes closest node; progress and carrot remain forward; terminal goal handoff persists");
  }

void guidanceDeviationAndHeight() {
  const Roads roads({{"a",{0,0,0}},{"b",{2000,0,0}},{"c",{4000,0,0}}},
                    {{0,1,2000,true},{1,2,2000,true}});
  const Roads::Knowledge knowledge{{1,1,1},{1,1},false};
  Roads::Guidance guidance;
  const Vec3 goal{5000,0,0};
  roads.updateGuidance(knowledge,{500,0,0},goal,guidance);
  roads.updateGuidance(knowledge,{1200,0,800},goal,guidance);
  require(!guidance.rebuilt,"eight metre cross-track deviation retains route");
  const Vec3 departed{1200,0,801};
  roads.updateGuidance(knowledge,departed,goal,guidance);
  require(guidance.rebuilt,"greater than eight metre deviation rebuilds route");
  const auto rebuilt = roads.path(knowledge,departed,goal);
  require(guidance.route.nodes==rebuilt.nodes && guidance.route.edges==rebuilt.edges,
          "deviation rebuilds familiar route from current pose");
  require(guidance.route.nodes==std::vector<size_t>({1,2}),"replan starts at currently closest familiar node");
  close(guidance.polyline.front().x,2000,"rebuilt polyline contains road nodes without pose connector");
  close(guidance.progressCm,0,"rebuilt projection starts at the new route's first node");

  Roads::Guidance offroad;
  roads.updateGuidance(knowledge,{500,0,500},goal,offroad);
  close(offroad.carrot.x,1100,"five metres off road preserves full six metre lookahead from projection");
  close(offroad.carrot.z,0,"five metres off road carrot remains on familiar path");
  roads.updateGuidance(knowledge,{500,0,900},goal,offroad);
  require(offroad.rebuilt,"nine metres off road rebuilds from current pose");
  close(offroad.progressCm,500,"off-road distance does not count as route progress");
  close(offroad.carrot.x,1100,"nine metres off road preserves full six metre lookahead from projection");
  close(offroad.carrot.z,0,"nine metres off road carrot remains on familiar path");

  // Coincident XZ road decks must not project onto the upper road's future.
  const Roads stacked({{"a",{0,0,0}},{"b",{2000,0,0}},
                       {"c",{2000,1000,0}},{"d",{0,1000,0}}},
                      {{0,1,2000,true},{1,2,1000,true},{2,3,2000,true}});
  const Roads::Knowledge all{{1,1,1,1},{1,1,1},false};
  Roads::Guidance deck;
  stacked.updateGuidance(all,{900,0,0},{0,1000,0},deck);
  close(deck.carrot.x,1500,"projection uses current road deck");
  close(deck.carrot.y,0,"height separates stacked familiar segments");
  require(!deck.beyondKnown,"upper route suffix is not skipped");

  const Roads empty({},{});
  Roads::Guidance unknown;
  empty.updateGuidance(empty.build({},{0,0,0}),{0,0,0},{300,0,400},unknown);
  require(unknown.beyondKnown && unknown.route.nodes.empty(),"empty knowledge hands off immediately");
  close((unknown.carrot-Vec3{300,0,400}).length(),0,"empty knowledge carrot is goal");
  empty.updateGuidance(empty.build({},{0,0,0}),{100,0,0},{300,0,400},unknown);
  require(!unknown.rebuilt && unknown.beyondKnown,"empty familiar path remains a stable goal handoff");
  std::puts("guidance deviation: >8 m rebuild from current pose; off-road carrot stays six metres ahead on road; stacked roads and empty graph handled");
  }

void guidanceHairpin() {
  // A U-shaped road: 20 m east, 10 m north, 40 m west. Near the start the far
  // leg is closer (3 m) than the first leg (7 m), but it is 47 m ahead in route.
  const Roads roads({{"a",{0,0,0}},{"b",{2000,0,0}},{"c",{2000,0,1000}},{"e",{-2000,0,1000}}},
                    {{0,1,2000,true},{1,2,1000,true},{2,3,4000,true}});
  const Roads::Knowledge knowledge{{1,1,1,1},{1,1,1},false};
  const Vec3 goal{-2500,0,1000};
  require(Roads::guidanceWindowCm==650.f,"window is the 6 m lookahead plus the largest 100 ms step");

  Roads::Guidance start;
  roads.updateGuidance(knowledge,{300,0,700},goal,start);
  require(start.rebuilt && start.route.nodes==std::vector<size_t>({0,1,2,3}),"route starts at the closest node");
  close(start.progressCm,300,"initial projection stays on the first leg, not on the closer far leg");
  close(start.carrot.x,900,"initial carrot is six metres along the first leg");
  close(start.carrot.z,0,"initial carrot is not behind the hairpin");

  Roads::Guidance guidance;
  roads.updateGuidance(knowledge,{0,0,100},goal,guidance);
  close(guidance.progressCm,0,"start on the first node");
  roads.updateGuidance(knowledge,{300,0,700},goal,guidance);
  require(!guidance.rebuilt,"three metres from the far leg is not a deviation");
  close(guidance.progressCm,300,"progress does not jump 47 m to the far leg");
  close(guidance.carrot.x,900,"carrot stays on the first leg");
  close(guidance.carrot.z,0,"carrot does not move behind the hairpin");
  roads.updateGuidance(knowledge,{300,0,100},goal,guidance);
  close(guidance.progressCm,300,"returning to the road keeps progress");

  // Ordinary following: the same projection and carrot as on the whole route.
  const std::vector<std::pair<Vec3,double>> walk = {
    {{800,0,50},800}, {{1400,0,-30},1400}, {{1950,0,0},1950}, {{2050,0,300},2300},
    {{1980,0,700},2700}, {{1700,0,1000},3300}, {{1200,0,950},3800}, {{600,0,1050},4400}};
  for(const auto& [position,progress]:walk) {
    roads.updateGuidance(knowledge,position,goal,guidance);
    require(!guidance.rebuilt,"following the hairpin keeps the route");
    close(guidance.progressCm,progress,"progress follows the route through the turn");
    }
  close(guidance.carrot.x,0,"carrot six metres ahead on the far leg");
  close(guidance.carrot.z,1000,"carrot on the far leg after the turn");

  // A later stretch beyond the window does not take progress, even when the
  // actor stands on it: only the >8 m deviation rebuilds the route.
  Roads::Guidance ahead;
  roads.updateGuidance(knowledge,{300,0,0},goal,ahead);
  for(int i=0; i<5; ++i) {
    roads.updateGuidance(knowledge,{2000,0,500},goal,ahead);
    require(!ahead.rebuilt,"standing on the route is not a deviation");
    close(ahead.progressCm,300,"a stretch 17 m ahead stays outside the window");
    }

  // A tight hairpin, 6 m wide: the turn enters the window. An actor that stands
  // between the legs, 5 m from the first and 1 m from the far one, keeps its
  // progress instead of creeping one window per decision onto the far leg.
  const Roads tight({{"a",{0,0,0}},{"b",{1000,0,0}},{"c",{1000,0,600}},{"e",{-2000,0,600}}},
                    {{0,1,1000,true},{1,2,600,true},{2,3,3000,true}});
  Roads::Guidance stand;
  tight.updateGuidance(knowledge,{0,0,50},{-2500,0,600},stand);
  require(stand.rebuilt && stand.route.nodes==std::vector<size_t>({0,1,2,3}),"tight route starts at its first node");
  tight.updateGuidance(knowledge,{300,0,100},{-2500,0,600},stand);
  tight.updateGuidance(knowledge,{700,0,100},{-2500,0,600},stand);
  for(int i=0; i<10; ++i) {
    tight.updateGuidance(knowledge,{700,0,500},{-2500,0,600},stand);
    require(!stand.rebuilt,"one metre from the far leg is not a deviation");
    close(stand.progressCm,700,"standing actor keeps progress on the first leg");
    }
  close(stand.carrot.x,1000,"carrot six metres ahead on the turn");
  close(stand.carrot.z,300,"carrot does not move onto the far leg");
  std::puts("guidance hairpin: far leg 3 m away does not take progress; following the turn matches the whole-route projection; standing actor never creeps onto the far leg");
  }
}

int main() {
  localAndStranger();
  unknownGoalAndAdd();
  acquisitionAndDeterminism();
  roadClassificationSeam();
  forwardGuidance();
  guidanceDeviationAndHeight();
  guidanceHairpin();
  std::puts("navigation roads contracts passed");
  }

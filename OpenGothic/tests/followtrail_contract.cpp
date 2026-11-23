#include "game/followtrail.h"
#include "game/navigationpassage.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

// World-free contract of the follow trail carrot. A toy floor is turned into the
// follower's own near memory (64 x 64, 25 cm, body frame) exactly as the memory
// lays it out; visibility is the caller's single eye-to-body test.
namespace {
using Tempest::Vec3;
using Source = FollowTrail::Source;

void require(bool condition, const char* message) {
  if(condition)
    return;
  std::fprintf(stderr,"FAIL: %s\n",message);
  std::exit(EXIT_FAILURE);
  }

struct Rect final { float x0, x1, z0, z1; };
struct Floor final {
  std::vector<Rect> walls;
  std::vector<Rect> movable;    // people or the object's own recent footprints
  bool seen = true;             // false: the follower has seen nothing yet
  bool checkerHoles = false;    // splat holes left by the body-frame rotation
  bool inside(const Vec3& p, float margin = 0) const {
    for(const auto& w:walls)
      if(p.x>=w.x0-margin && p.x<=w.x1+margin && p.z>=w.z0-margin && p.z<=w.z1+margin)
        return true;
    return false;
    }
  bool crosses(const Vec3& a, const Vec3& b) const {
    const float length = std::hypot(b.x-a.x,b.z-a.z);
    const int steps = std::max(1,int(std::ceil(length)));
    for(int k=0; k<=steps; ++k)
      if(inside(a+(b-a)*(float(k)/float(steps))))
        return true;
    return false;
    }
  NavigationMemory::View view(const Vec3& position, const Vec3& forward) const {
    NavigationMemory::View result;
    result.pose.position = position;
    result.pose.forward = forward;
    result.near.size = 64;
    result.near.cellCm = 25;
    result.near.cells.resize(64*64);
    const Vec3 right(-forward.z,0,forward.x);
    for(size_t z=0; z<64; ++z)
      for(size_t x=0; x<64; ++x) {
        const float r = (float(x)+.5f-32.f)*25.f, f = (float(z)+.5f-32.f)*25.f;
        const Vec3 world = position+right*r+forward*f;
        auto& cell = result.near.cells[z*64+x];
        if(!seen)
          continue;
        if(inside(world,12.5f)) {cell.confidence=1;cell.wall=1;continue;}
        for(const auto& m:movable)
          if(world.x>=m.x0 && world.x<=m.x1 && world.z>=m.z0 && world.z<=m.z1) cell.movable=1;
        if(checkerHoles && (x+z)%2==0)
          continue;
        cell.confidence = 1;cell.free = 1;
        }
    return result;
    }
  };

const Vec3 East(1,0,0);

void moveAlong(const std::vector<Vec3>& path, size_t& leg, Vec3& position, float step) {
  while(step>0 && leg<path.size()) {
    const auto delta = path[leg]-position;
    const float length = delta.length();
    if(length<=step) {position=path[leg];step-=length;++leg;continue;}
    position += delta*(step/length);
    step = 0;
    }
  }

double distance(const Vec3& a, const Vec3& b) { return double((a-b).length()); }

// The point lookaheadCm along the remembered trail from the follower's projection.
Vec3 lookahead(const FollowTrail& trail) {
  const double arc = trail.progressCm()+FollowTrail::lookaheadCm;
  for(size_t i=1; i<trail.points().size(); ++i) {
    const auto& a = trail.points()[i-1];
    const auto& b = trail.points()[i];
    if(arc<=b.arcCm)
      return a.position+(b.position-a.position)*float((arc-a.arcCm)/(b.arcCm-a.arcCm));
    }
  return trail.points().back().position;
  }

// The leader walks around the end of a wall and stops behind it. The follower
// steps toward its carrot and never sees through the wall.
void wallDetour() {
  Floor floor;
  floor.walls.push_back({390,410,-400,100});
  const std::vector<Vec3> path = {{250,0,0},{300,0,250},{650,0,250},{650,0,-300}};
  FollowTrail trail;
  Vec3 object = path[0], follower(-150,0,0), heading = East;
  size_t leg = 1;
  size_t unseen = 0, unseenTrail = 0, direct = 0;
  for(int step=0; step<260; ++step) {
    moveAlong(path,leg,object,60.f);
    const Vec3 eye = follower;
    const bool visible = !floor.crosses(eye,object);
    trail.observe(object,visible);
    if(!visible) {
      require(!trail.points().empty(),"the trail started while the leader was visible");
      require(distance(trail.points().back().position,object)>1,"an unseen position never enters the trail");
      }
    const auto result = trail.guide(follower,object,floor.view(follower,heading));
    require(result.source!=Source::Line && result.source!=Source::Unseen && result.source!=Source::Heading &&
            result.source!=Source::LastSeen,"a visible trail needs no straight fallback here");
    require(!floor.crosses(follower,result.carrot),"the carrot never crosses the wall");
    if(result.source!=Source::Hold)
      require(distance(result.carrot,object)>=FollowTrail::standoffCm-.5,"the carrot keeps the standoff");
    direct += result.source==Source::Direct;
    if(!visible) {
      ++unseen;
      unseenTrail += result.source==Source::Trail || result.source==Source::Cut;
      }
    auto delta = result.carrot-follower;
    delta.y = 0;
    const float length = delta.length();
    if(length>1) {
      heading = delta*(1.f/length);
      follower += delta*(std::min(20.f,length)/length);
      }
    require(!floor.inside(follower),"the follower never enters the wall");
    }
  std::printf("wall detour: unseen %zu decisions, %zu on the trail, direct %zu, final distance %.1f cm\n",
              unseen,unseenTrail,direct,distance(follower,object));
  require(unseen>0,"the leader was hidden behind the wall for a while");
  require(unseenTrail==unseen,"while the leader is hidden the follower goes by its trail");
  require(direct>0,"the straight cut is taken where the near memory sees it free");
  const double final = distance(follower,object);
  require(final>=200 && final<=500,"the follower ends in the 2-5 m band behind the wall");
  }

// The same leader hidden behind the wall, frozen: one decision from the start.
void hiddenLeader() {
  Floor floor;
  floor.walls.push_back({390,410,-400,100});
  const Vec3 follower(0,0,0), hidden(650,0,-300);
  FollowTrail trail;
  for(const auto& seen:{Vec3(250,0,0),Vec3(300,0,250),Vec3(650,0,250)}) {
    const auto from = trail.points().empty() ? seen : trail.points().back().position;
    for(int k=1; k<=20; ++k)
      trail.observe(from+(seen-from)*(float(k)/20.f),true);
    }
  trail.observe(hidden,false);
  require(floor.crosses(follower,follower+(hidden-follower)*.6f),"the straight line to the hidden leader crosses the wall");
  require(distance(trail.points().back().position,Vec3(650,0,250))<1,"the last seen point is the trail end");

  const auto known = trail.guide(follower,hidden,floor.view(follower,East));
  require(known.source==Source::Trail || known.source==Source::Cut,"a hidden leader is followed along the trail");
  require(!floor.crosses(follower,known.carrot),"with the wall in memory the carrot stays clear of it");

  // No memory at all: nothing is seen, so no cut and no pullback; the lookahead point on the trail.
  floor.seen = false;
  FollowTrail blind = trail;
  const auto unseen = blind.guide(follower,hidden,floor.view(follower,East));
  require(unseen.source==Source::Trail,"without memory the follower keeps to the trail");
  const Vec3 expected = lookahead(blind);
  require(distance(unseen.carrot,expected)<1,"the trail carrot is the lookahead point from the projection");
  }

// Open floor: a straight cut is taken only where the near memory sees it free.
void straightCut() {
  const std::vector<Vec3> path = {{300,0,0},{300,0,500},{700,0,500},{700,0,0}};
  FollowTrail trail;
  Vec3 object = path[0];
  size_t leg = 1;
  trail.observe(object,true);
  while(leg<path.size()) {moveAlong(path,leg,object,20.f);trail.observe(object,true);}
  const Vec3 follower(0,0,0);
  const float gap = (object-follower).length();
  const Vec3 straight = follower+(object-follower)*((gap-FollowTrail::standoffCm)/gap);

  Floor open;
  FollowTrail seen = trail;
  const auto cut = seen.guide(follower,object,open.view(follower,East));
  require(cut.source==Source::Direct && distance(cut.carrot,straight)<.5,"a free straight passage is cut directly");

  Floor holes;holes.checkerHoles=true;
  FollowTrail sparse = trail;
  require(sparse.guide(follower,object,holes.view(follower,East)).source==Source::Direct,
          "rotation holes in seen ground do not block a free cut");

  Floor blocked;blocked.walls.push_back({200,220,-150,150});
  FollowTrail around = trail;
  const auto detour = around.guide(follower,object,blocked.view(follower,East));
  require(detour.source==Source::Cut || detour.source==Source::Trail,"a seen wall on the straight line forbids the cut");
  require(!blocked.crosses(follower,detour.carrot),"the detour carrot does not cross the seen wall");
  require(detour.carrot.z>100,"the carrot goes around by the trail");

  Floor dark;dark.seen=false;
  FollowTrail unseen = trail;
  const auto blind = unseen.guide(follower,object,dark.view(follower,East));
  require(blind.source==Source::Trail && distance(blind.carrot,lookahead(unseen))<1 && blind.carrot.z>490,
          "unseen ground: the lookahead point along the U, not the straight line");

  FollowTrail close = trail;
  const auto hold = close.guide(object+Vec3(-200,0,0),object,open.view(follower,East));
  require(hold.source==Source::Hold && distance(hold.carrot,object+Vec3(-200,0,0))<.01,
          "within the standoff the follower holds");
  }

// The follower reaches the last seen point and still does not see the object.
void lostAtTrailEnd() {
  Floor dark;dark.seen=false;
  FollowTrail trail;
  for(float x=0; x<=500; x+=25)
    trail.observe(Vec3(x,0,0),true);
  const Vec3 hidden(500,0,1500);
  trail.observe(hidden,false);
  const auto far = trail.guide(Vec3(100,0,0),hidden,dark.view(Vec3(100,0,0),East));
  require(far.source==Source::Trail && distance(far.carrot,Vec3(500,0,0))<1,"the last seen point while it is ahead");
  // Nothing seen between: the companion goes toward the object it hears.
  FollowTrail open = trail;
  open.observe(hidden,false);
  const auto at = open.guide(Vec3(480,0,0),hidden,dark.view(Vec3(480,0,0),East));
  require(at.source==Source::Unseen && open.lost(),"at the last seen point the object is followed by hearing");
  open.observe(hidden,false);
  const auto latched = open.guide(Vec3(480,0,150),hidden,dark.view(Vec3(480,0,150),East));
  require(latched.source==Source::Unseen,"the loss holds until the object is seen again");
  open.observe(Vec3(500,0,1400),true);
  require(!open.lost(),"seeing the object again resumes the trail");

  // A seen wall between the last seen point and the hidden object (review F1/F7):
  // never through it; where the object was last seen heading instead.
  Floor wall;wall.walls.push_back({400,600,100,120});
  for(const bool seenFloor:{true,false}) {
    wall.seen = true;
    auto view = wall.view(Vec3(480,0,0),East);
    if(!seenFloor)
      for(auto& cell:view.near.cells)
        if(cell.wall==0) cell = {};
    FollowTrail blocked = trail;
    blocked.observe(hidden,false);
    const auto result = blocked.guide(Vec3(480,0,0),hidden,view);
    require(blocked.lost() && result.source==Source::Heading,"a closed straight carrot gives way to the last heading");
    require(!wall.crosses(Vec3(480,0,0),result.carrot),"a lost carrot never crosses the seen wall");
    require(distance(result.carrot,Vec3(1100,0,0))<1,"the heading continues 6 m from the last seen point");
    }
  // The heading is closed too: around the wall by hearing, as for an object
  // never seen (the direction toward it turned aside up to 90 degrees).
  Floor boxed;boxed.walls.push_back({400,600,100,120});boxed.walls.push_back({700,720,-300,300});
  FollowTrail cornered = trail;
  cornered.observe(hidden,false);
  const auto aside = cornered.guide(Vec3(480,0,0),hidden,boxed.view(Vec3(480,0,0),East));
  require(aside.source==Source::Aside && !boxed.crosses(Vec3(480,0,0),aside.carrot) &&
          distance(aside.carrot,Vec3(480,0,0))>=FollowTrail::pullbackMinCm && aside.carrot.x<400 &&
          distance(aside.carrot,hidden)>=FollowTrail::standoffCm,
          "both closed: the lost follower turns aside past the end of the seen wall");
  // The switch restores the stand at the last seen point.
  FollowTrail standing = trail;
  standing.setLostAside(false);
  standing.observe(hidden,false);
  const auto stood = standing.guide(Vec3(480,0,0),hidden,boxed.view(Vec3(480,0,0),East));
  require(stood.source==Source::LastSeen && distance(stood.carrot,Vec3(500,0,0))<1,
          "lost aside off: the last seen point");
  // Closed within 90 degrees on both sides as well: only now the last seen point.
  boxed.walls.push_back({200,220,-300,300});
  FollowTrail walled = trail;
  walled.observe(hidden,false);
  const auto last = walled.guide(Vec3(480,0,0),hidden,boxed.view(Vec3(480,0,0),East));
  require(last.source==Source::LastSeen && distance(last.carrot,Vec3(500,0,0))<1,"all closed: the last seen point");
  // Never seen, behind a seen wall: toward the object only up to the wall.
  FollowTrail never;
  never.observe(hidden,false);
  require(never.points().empty() && never.guide(Vec3(),hidden,dark.view(Vec3(),East)).source==Source::Unseen,
          "an object never seen is followed by hearing");
  Floor front;front.walls.push_back({-200,200,600,620});
  const auto around = never.guide(Vec3(),hidden,front.view(Vec3(),East));
  require(around.source==Source::Unseen && around.detour && !front.crosses(Vec3(),around.carrot),
          "hearing never leads through a seen wall");
  require(distance(around.carrot,Vec3())>=FollowTrail::pullbackMinCm && around.carrot.x>200,
          "hearing turns aside past the end of the seen wall");
  // A seen wall across the whole near map: the carrot stays on the near side.
  Floor shut;shut.walls.push_back({-800,800,300,320});
  const auto stopped = never.guide(Vec3(),hidden,shut.view(Vec3(),East));
  require(stopped.source==Source::Unseen && stopped.detour && !shut.crosses(Vec3(),stopped.carrot) &&
          stopped.carrot.z<300,"a long seen wall: the carrot stays on the near side");
  }

// The trail loops back past the object (review F2 and F8): no trail or cut
// carrot may come nearer than 2.5 m to it.
void trailLoops() {
  Floor dark;dark.seen=false;
  const auto loop = [&](const std::vector<Vec3>& path, const Vec3& follower, const char* message) {
    FollowTrail trail;
    Vec3 object = path[0];
    size_t leg = 1;
    trail.observe(object,true);
    while(leg<path.size()) {moveAlong(path,leg,object,25.f);trail.observe(object,true);}
    const auto result = trail.guide(follower,object,dark.view(follower,East));
    require(result.source==Source::Trail || result.source==Source::Cut || result.source==Source::Line,message);
    require(distance(result.carrot,object)>=FollowTrail::standoffCm-.5,message);
    return result;
    };
  const auto f2 = loop({{100,0,0},{1100,0,0},{1100,0,600},{500,0,600},{500,0,0}},Vec3(0,0,0),
                       "F2: the loop past the object keeps the 2.5 m standoff");
  require(f2.source==Source::Trail && distance(f2.carrot,Vec3(250,0,0))<1 && f2.loopLimited,
          "F2: the carrot stops where the trail first enters the standoff circle");
  const auto f8 = loop({{0,0,0},{1000,0,0},{1000,0,1000},{700,0,1000},{700,0,150}},Vec3(100,0,0),
                       "F8: the loop past the object keeps the 2.5 m standoff");
  require(f8.source==Source::Trail && distance(f8.carrot,Vec3(500,0,0))<1 && f8.loopLimited,
          "F8: the carrot stops where the trail first enters the standoff circle");
  }

// The object walks toward the follower: no trail point ahead is 2.5 m from it.
void comingToward() {
  Floor dark;dark.seen=false;
  FollowTrail trail;
  for(float x=1000; x>=600; x-=25)
    trail.observe(Vec3(x,0,0),true);
  const Vec3 follower(0,0,0), object(600,0,0);
  const auto result = trail.guide(follower,object,dark.view(follower,East));
  require(result.source==Source::Line && distance(result.carrot,Vec3(350,0,0))<.5,
          "an object coming toward the follower leaves the straight carrot");
  }

// The object's own footprints along its trail are movable evidence. They must
// not pull the trail carrot back to the follower's feet (the planner would hold).
void footprintsDoNotTrap() {
  FollowTrail trail;
  for(float x=0; x<=1500; x+=25)
    trail.observe(Vec3(x,0,0),true);
  Floor marked;marked.movable.push_back({-100,1600,-40,40});
  const Vec3 follower(-50,0,0), object(1500,0,0);
  const auto result = trail.guide(follower,object,marked.view(follower,East));
  require(result.source==Source::Cut && distance(result.carrot,follower)>FollowTrail::lookaheadCm,
          "movable footprints neither block the cut nor trap the carrot");
  }

// A seen wall at a corner of the trail pulls the carrot back along the trail,
// to a point the follower can head for, never to its feet.
void cornerPullback() {
  FollowTrail trail;
  const std::vector<Vec3> path = {{0,0,0},{100,0,0},{100,0,300},{700,0,300}};
  Vec3 object = path[0];
  size_t leg = 1;
  trail.observe(object,true);
  while(leg<path.size()) {moveAlong(path,leg,object,20.f);trail.observe(object,true);}
  Floor floor;floor.seen=false;floor.walls.push_back({150,700,50,250});
  Floor walls = floor;
  walls.seen = true;
  // Only the wall is known: every other cell is unseen, so neither direct nor cut.
  auto view = walls.view(Vec3(),East);
  for(auto& cell:view.near.cells)
    if(cell.wall==0) cell = {};
  const auto result = trail.guide(Vec3(),object,view);
  require(result.source==Source::Trail,"unseen ground with a seen wall: the trail carrot");
  require(!walls.crosses(Vec3(),result.carrot),"the pulled-back carrot does not cross the seen wall");
  require(distance(result.carrot,Vec3())>=FollowTrail::pullbackMinCm,"the pulled-back carrot stays at least 1 m ahead");
  }

void shortMemoryAndSpacing() {
  FollowTrail trail;
  trail.observe(Vec3(0,0,0),true);
  trail.observe(Vec3(10,0,0),true);
  require(trail.points().size()==1,"motion below the spacing adds no point");
  for(float x=0; x<=10000; x+=30)
    trail.observe(Vec3(x,0,0),true);
  const auto& points = trail.points();
  require(points.back().arcCm-points.front().arcCm<=FollowTrail::memoryCm+FollowTrail::spacingCm*2,
          "the trail keeps only a short memory");
  require(points.back().position.x>9900,"the newest point is kept");
  }

void passageChecks() {
  Floor open;
  const auto view = open.view(Vec3(),East);
  require(FollowTrail::nearFree(view,Vec3(),Vec3(500,0,0)),"seen free floor is a free passage");
  require(!FollowTrail::nearFree(view,Vec3(),Vec3(900,0,0)),"beyond the near map nothing is seen free");
  require(FollowTrail::nearClear(view,Vec3(),Vec3(900,0,0)),"beyond the near map nothing blocks either");
  Floor wall;wall.walls.push_back({300,320,10,40});
  const auto side = wall.view(Vec3(),East);
  require(!FollowTrail::nearFree(side,Vec3(),Vec3(500,0,0)) && !FollowTrail::nearClear(side,Vec3(),Vec3(500,0,0)),
          "a hazard within the corridor blocks the passage");
  require(FollowTrail::nearFree(side,Vec3(),Vec3(500,0,-120)),"a hazard outside the corridor does not");
  Floor dark;dark.seen=false;
  require(!FollowTrail::nearFree(dark.view(Vec3(),East),Vec3(),Vec3(300,0,0)) &&
          FollowTrail::nearClear(dark.view(Vec3(),East),Vec3(),Vec3(300,0,0)),"unknown ground is clear but not free");
  // The body frame: right is (-forward.z, 0, forward.x).
  const auto north = wall.view(Vec3(),Vec3(0,0,1));
  require(!FollowTrail::nearClear(north,Vec3(),Vec3(500,0,45)),"the rotated memory still holds the wall");

  // Road guidance reads the same near map, then the far map (2 m cells) beyond it.
  using Verdict = NavigationPassage::Verdict;
  const auto check = [](const NavigationMemory::View& memory, const Vec3& to) {
    return NavigationPassage::check(memory,Vec3(),to,FollowTrail::corridorCm,FollowTrail::footprintCm);
    };
  require(check(view,Vec3(500,0,0))==Verdict::Free,"seen free near ground is free for the roads");
  require(check(side,Vec3(500,0,0))==Verdict::Closed,"a seen wall closes the road passage");
  require(check(dark.view(Vec3(),East),Vec3(300,0,0))==Verdict::Unknown,"unseen ground is unknown");
  require(check(view,Vec3(2000,0,0))==Verdict::Unknown,"without a far map nothing beyond the near map is seen");
  // Far map in the body frame of an east-facing walker: forward is +x, right is +z.
  auto far = view;
  far.far.size = 100;
  far.far.cellCm = 200;
  far.far.cells.resize(100*100);
  const auto farCell = [&](float x, float z) -> NavigationMemory::Cell& {
    return far.far.cells[size_t(std::floor(x/200.f+50.f))*100+size_t(std::floor(z/200.f+50.f))];
    };
  for(auto& cell:far.far.cells) {cell.confidence=1;cell.free=1;}
  require(check(far,Vec3(2000,0,0))==Verdict::Free,"seen free far ground extends a free passage");
  farCell(1300,0) = {};
  farCell(1300,0).confidence = 1;farCell(1300,0).wall = 1;
  require(check(far,Vec3(2000,0,0))==Verdict::Closed,"a far wall cell without free ground closes the passage");
  require(check(far,Vec3(2000,0,700))==Verdict::Free,"a far wall cell beside the passage does not");
  farCell(1300,0).free = 1;
  require(check(far,Vec3(2000,0,0))==Verdict::Unknown,"a far cell with both a wall and free ground is not decisive");
  require(check(far,Vec3(25000,0,0))==Verdict::Unknown,"beyond the far map nothing is seen");
  }
}

int main() {
  wallDetour();
  hiddenLeader();
  straightCut();
  lostAtTrailEnd();
  trailLoops();
  comingToward();
  footprintsDoNotTrap();
  cornerPullback();
  shortMemoryAndSpacing();
  passageChecks();
  std::puts("followtrail contract: ok");
  return 0;
  }

#include "navkeyteacher.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using Point = NavMesh::Point;
void check(bool condition,const char* message) {
  if(!condition) throw std::runtime_error(message);
  }
NavMesh::Path route(std::initializer_list<Point> corners) {
  NavMesh::Path p;
  p.complete=true;
  p.corners=corners;
  return p;
  }
void follow(NavKeyTeacher::State body,const NavMesh::Path& path,const Point& goal) {
  for(unsigned tick=0;tick<1600;++tick) {
    const auto decision=NavKeyTeacher::decide(body,path,goal);
    if(decision.arrived) return;
    body.yawDegrees-=decision.keys.turn*9.f;
    const float radians=body.yawDegrees*3.14159265358979323846f/180.f;
    const float speed=decision.keys.forward*(decision.keys.walk?.85f:3.f);
    body.velocity={std::cos(radians)*speed,0,std::sin(radians)*speed};
    body.position[0]+=body.velocity[0]*.1f;
    body.position[2]+=body.velocity[2]*.1f;
    }
  throw std::runtime_error("memoryless keyboard teacher did not finish the little graph");
  }
}

int main() {
  try {
    NavKeyTeacher::State body;
    const auto straight=route({{0,0,0},{10,0,0}});
    auto d=NavKeyTeacher::decide(body,straight,{10,0,0});
    check(d.pathValid && d.keys.walk && d.keys.forward==1 && d.keys.turn==0,
          "ordinary route must use forward walking");
    body.yawDegrees=90;
    d=NavKeyTeacher::decide(body,straight,{10,0,0});
    check(d.keys.turn==1 && d.keys.forward==0,"positive turn must reduce NPC yaw");
    body.yawDegrees=-90;
    check(NavKeyTeacher::decide(body,straight,{10,0,0}).keys.turn==-1,
          "negative turn must increase NPC yaw");
    body.yawDegrees=179;
    const auto wrap=route({{0,0,0},{-10,0,-.1f}});
    check(NavKeyTeacher::decide(body,wrap,wrap.corners.back()).keys.turn==0,
          "heading wrap must use the shortest rotation with a digital deadband");

    const auto elbow=route({{0,0,0},{4,0,0},{4,0,5},{9,0,5}});
    body={};
    follow(body,elbow,elbow.corners.back());
    // The same pure policy labels arbitrary off-route student poses and rotations.
    for(float offset:{-5.f,-2.f,1.f,3.f,5.f}) {
      body={};body.position={2,0,offset};body.yawDegrees=offset*43.f;
      follow(body,elbow,elbow.corners.back());
      }
    body={};body.position={4.1f,0,.15f};body.yawDegrees=90;
    d=NavKeyTeacher::decide(body,elbow,elbow.corners.back());
    check(d.hint.cornerIndex>=2 && d.keys.forward==1,"a passed corner must not pull the body backwards");

    const Point carrot={6,0,6}, finalGoal={20,0,6};
    const auto local=route({{0,0,0},{6,0,0},carrot});
    body={};body.yawDegrees=45;
    d=NavKeyTeacher::decideV2(body,local,finalGoal,carrot,true);
    check(d.pathValid && !d.arrived && d.keys.walk && d.keys.forward==1 && d.keys.turn==0 &&
          d.hint.nextCorner==carrot,
          "v2 clear ground must steer to the learner carrot instead of a mesh corner");
    const auto v1Local=NavKeyTeacher::decide(body,local,finalGoal);
    d=NavKeyTeacher::decideV2(body,local,finalGoal,carrot,false);
    check(d.hint.nextCorner==v1Local.hint.nextCorner && d.keys.turn==v1Local.keys.turn &&
          d.keys.forward==v1Local.keys.forward,
          "v2 obstacle avoidance must use the local Detour corner with v1 digital steering");
    body={};body.position={5.6f,0,6};
    d=NavKeyTeacher::decideV2(body,local,finalGoal,carrot,true);
    check(!d.arrived && d.keys.forward==1 && d.hint.nextCorner==carrot,
          "reaching a local carrot must not satisfy final-goal arrival");
    body.position=finalGoal;
    check(NavKeyTeacher::decideV2(body,local,finalGoal,carrot,true).arrived,
          "v2 must retain final-goal arrival even if the local carrot differs");
    for(float yaw:{-9.f,9.f}) {
      body={};body.yawDegrees=yaw;
      check(NavKeyTeacher::decideV2(body,straight,{20,0,0},{6,0,0},true).keys.turn==0,
            "v2 ground steering must leave errors inside ten degrees unlabelled");
      check(NavKeyTeacher::decideV2(body,straight,{20,0,0},{6,0,0},false).keys.turn==0,
            "v2 local Detour steering must share the ten-degree ground deadband");
      }
    for(float yaw:{-11.f,11.f}) {
      body={};body.yawDegrees=yaw;
      check(NavKeyTeacher::decideV2(body,straight,{20,0,0},{6,0,0},true).keys.turn==(yaw>0 ? 1.f : -1.f),
            "v2 ground errors outside ten degrees must turn toward the carrot");
      }

    for(const auto type:{NavMesh::LinkType::Jump,NavMesh::LinkType::Climb,NavMesh::LinkType::Drop}) {
      auto linked=route({{0,0,0},{2,0,0},{4,1,0},{7,1,0}});
      NavMesh::Path::Transition link;
      link.start=linked.corners[1];link.end=linked.corners[2];link.type=type;link.id=42;link.cornerIndex=1;
      linked.transitions.push_back(link);
      body={};body.position=link.start;
      d=NavKeyTeacher::decide(body,linked,linked.corners.back());
      check(d.hint.onTransition && d.hint.transitionId==42 && !d.keys.walk && d.keys.forward==1,
            "link takeoff must aim at its directed landing and use running input");
      check(d.keys.jump==(type==NavMesh::LinkType::Jump) && d.keys.climb==(type==NavMesh::LinkType::Climb),
            "jump/climb/drop links must retain their keyboard action");
      const auto v2=NavKeyTeacher::decideV2(body,linked,{20,1,0},linked.corners.back(),false);
      check(v2.hint.onTransition && v2.hint.transitionId==d.hint.transitionId &&
            v2.keys.jump==d.keys.jump && v2.keys.climb==d.keys.climb &&
            v2.keys.walk==d.keys.walk && v2.keys.forward==d.keys.forward,
            "v2 local paths must preserve v1 jump/climb/drop transitions");
      if(type!=NavMesh::LinkType::Drop) {
        body={};body.position={2.05f,0,.35f};
        d=NavKeyTeacher::decideV2(body,linked,{20,1,0},linked.corners.back(),false);
        check(d.pathValid && d.hint.transitionId==42 && !d.hint.onTransition &&
              d.hint.nextCorner==link.start && d.keys.walk && !d.keys.jump && !d.keys.climb,
              "a grounded side approach beyond takeoff must walk back to its exact start, not traverse the corridor");
        body.position={1.8f,0,0};
        d=NavKeyTeacher::decide(body,linked,linked.corners.back());
        check(d.hint.nextCorner==link.start && d.keys.walk && d.keys.forward==1 &&
              !d.hint.onTransition && !d.keys.jump && !d.keys.climb,
              "twenty centimetres from takeoff must remain a walking approach, not an early native action");
        for(float offset:{-.09f,.08f}) {
          body={};body.position={2.f+offset,0,0};
          d=NavKeyTeacher::decide(body,linked,linked.corners.back());
          check(d.hint.onTransition && !d.keys.walk && d.keys.forward==1 && d.keys.turn==0 &&
                d.keys.jump==(type==NavMesh::LinkType::Jump) && d.keys.climb==(type==NavMesh::LinkType::Climb),
                "the precise takeoff window must allow a small undershoot or overshoot and press run plus action");
        }
        body={};body.position=link.start;body.standing=false;
        d=NavKeyTeacher::decide(body,linked,linked.corners.back());
        check(d.hint.onTransition && !d.keys.walk && d.keys.forward==0 && !d.keys.jump && !d.keys.climb,
              "both native actions must release movement and wait for standing at the takeoff");
        body.standing=true;body.yawDegrees=4.8f;
        d=NavKeyTeacher::decide(body,linked,linked.corners.back());
        check(d.keys.turn!=0 && d.keys.forward==0 && !d.keys.walk && !d.keys.jump && !d.keys.climb,
              "turning and a native action must not be requested in the same takeoff decision");
        body={};body.position={2.2f,0,0};body.jumping=true;body.supported=false;
        d=NavKeyTeacher::decide(body,linked,linked.corners.back());
        check(d.hint.onTransition && d.hint.nextCorner==link.end && !d.keys.walk &&
              !d.keys.jump && !d.keys.climb,
              "a native jump animation must retain landing direction before inAir without retriggering");
        body={};body.position=link.start;
      }
      if(type==NavMesh::LinkType::Climb) {
        body.standing=false;
        d=NavKeyTeacher::decide(body,linked,linked.corners.back());
        check(d.hint.onTransition && d.keys.forward==0 && !d.keys.jump && !d.keys.climb,
              "moving at a climb takeoff must release forward until the native standing gate opens");
        body.standing=true;
        d=NavKeyTeacher::decide(body,linked,linked.corners.back());
        check(d.keys.forward==1 && d.keys.climb && !d.keys.jump,
              "standing at a climb takeoff must resume forward plus climb without a remembered phase");
        }
      body.yawDegrees=9;
      d=NavKeyTeacher::decide(body,linked,linked.corners.back());
      check(type==NavMesh::LinkType::Drop || (d.keys.forward==0 && !d.keys.jump && !d.keys.climb),
            "jump/climb takeoff must wait for alignment instead of walking off the edge");
      body.yawDegrees=0;
      body.position={3,.5f,0};body.supported=false;body.inAir=true;
      d=NavKeyTeacher::decide(body,linked,linked.corners.back());
      check(d.hint.onTransition && !d.keys.jump && !d.keys.climb && !d.replan,
            "midair state must reconstruct the transition without jump history");
      body={};body.position=link.end;
      d=NavKeyTeacher::decide(body,linked,linked.corners.back());
      check(!d.hint.onTransition && d.hint.cornerIndex>=3 && d.keys.walk,
            "supported landing must advance without historical transition flags");
      }

    auto dropPath=route({{0,0,0},{2,0,0},{2.8f,-2,0},{5,-2,0}});
    NavMesh::Path::Transition drop;
    drop.start=dropPath.corners[1];drop.end=dropPath.corners[2];
    drop.type=NavMesh::LinkType::Drop;drop.id=44;drop.cornerIndex=1;
    dropPath.transitions.push_back(drop);
    body={};body.position={2.05f,0,.29f};body.velocity={1.f,0,0};
    d=NavKeyTeacher::decide(body,dropPath,dropPath.corners.back());
    check(d.hint.transitionId==44 && !d.hint.onTransition && d.hint.nextCorner==drop.start &&
          d.keys.walk && !d.keys.jump && !d.keys.climb,
          "a drop side approach must walk to the precise start despite positive progress along the link");
    body={};body.position=drop.start;body.yawDegrees=17.f;
    d=NavKeyTeacher::decide(body,dropPath,dropPath.corners.back());
    check(d.hint.onTransition && d.keys.forward==0 && d.keys.turn!=0 && !d.keys.walk,
          "a drop must align in run-mode idle before its first forward action");
    body.yawDegrees=0;body.standing=false;
    d=NavKeyTeacher::decide(body,dropPath,dropPath.corners.back());
    check(d.keys.forward==0 && !d.keys.walk,
          "a stationary drop must wait for standing at its precise takeoff");
    body.standing=true;
    d=NavKeyTeacher::decide(body,dropPath,dropPath.corners.back());
    check(d.hint.onTransition && d.keys.forward==1 && !d.keys.walk && !d.keys.jump && !d.keys.climb,
          "an aligned standing drop must start running without a jump or climb action");
    for(float progress:{.06f,.20f}) {
      body={};body.position={2.f+progress,0,0};body.velocity={1.f,0,0};body.standing=false;
      d=NavKeyTeacher::decide(body,dropPath,dropPath.corners.back());
      check(d.hint.onTransition && d.hint.nextCorner==drop.end && d.keys.forward==1 && !d.keys.walk,
            "an ongoing narrow-corridor drop must continue even inside takeoff after standing closes");
    }
    body.velocity={-1.f,0,0};
    d=NavKeyTeacher::decide(body,dropPath,dropPath.corners.back());
    check(!d.hint.onTransition && d.hint.nextCorner==drop.start && d.keys.walk,
          "moving back toward a drop's start is not an ongoing departure");

    auto shortLink=route({{0,0,0},{2,0,0},{2.4f,0,0},{5,0,0}});
    NavMesh::Path::Transition shortJump;
    shortJump.start=shortLink.corners[1];shortJump.end=shortLink.corners[2];
    shortJump.type=NavMesh::LinkType::Jump;shortJump.id=43;shortJump.cornerIndex=1;
    shortLink.transitions.push_back(shortJump);
    body={};body.position=shortJump.start;
    d=NavKeyTeacher::decide(body,shortLink,shortLink.corners.back());
    check(d.hint.onTransition && d.keys.jump && d.hint.transitionId==43,
          "a short link's landing radius must not classify its takeoff as an already completed crossing");

    body={};body.position={10,0,0};
    check(NavKeyTeacher::decide(body,straight,{10,0,0}).arrived,"goal radius must stop the teacher");
    body={};NavMesh::Path missing;
    d=NavKeyTeacher::decide(body,missing,{10,0,0});
    check(!d.pathValid && d.replan && d.keys.forward==0,"missing path must request replanning without invented movement");
    body.position[0]=std::numeric_limits<float>::quiet_NaN();
    d=NavKeyTeacher::decide(body,straight,{10,0,0});
    check(d.replan && std::isfinite(d.keys.turn),"non-finite body must not create non-finite actions");
    body={};
    d=NavKeyTeacher::decideV2(body,straight,{10,0,0},{std::numeric_limits<float>::quiet_NaN(),0,0},true);
    check(d.replan && !d.pathValid && d.keys.forward==0,
          "invalid v2 carrot must request replanning without movement");
    std::cout<<"navkeyteacher contract: digital yaw, walking, little graph, five off-route states, passed corner, v2 shared carrot and local detour, jump/climb/drop, precise side approach, stationary native gates, takeoff alignment, midair reconstruction, short-link landing, invalid input passed\n";
    return 0;
  } catch(const std::exception& error) {
    std::cerr<<"navkeyteacher contract: "<<error.what()<<'\n';
    return 1;
    }
  }

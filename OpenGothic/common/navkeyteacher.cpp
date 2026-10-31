#include "navkeyteacher.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
using Point = NavMesh::Point;
constexpr float pi = 3.14159265358979323846f;

bool finite(const Point& p) {
  return std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]);
  }
float horizontal(const Point& a, const Point& b) {
  return std::hypot(a[0]-b[0],a[2]-b[2]);
  }
float distance(const Point& a, const Point& b) {
  return std::hypot(horizontal(a,b),a[1]-b[1]);
  }
float along(const Point& p, const Point& a, const Point& b) {
  const float x=b[0]-a[0], z=b[2]-a[2], length=x*x+z*z;
  return length>1e-6f ? ((p[0]-a[0])*x+(p[2]-a[2])*z)/length : 0.f;
  }
Point lerp(const Point& a, const Point& b, float t) {
  return {a[0]+(b[0]-a[0])*t,a[1]+(b[1]-a[1])*t,a[2]+(b[2]-a[2])*t};
  }
bool close(const Point& p, const Point& q, float radius=.22f, float height=1.f) {
  return horizontal(p,q)<=radius && std::abs(p[1]-q[1])<=height;
  }
float remaining(const Point& p,const NavMesh::Path& path,size_t corner) {
  float result=distance(p,path.corners[corner]);
  for(size_t i=corner+1;i<path.corners.size();++i)
    result+=distance(path.corners[i-1],path.corners[i]);
  return result;
  }
float angle(const NavKeyTeacher::State& body,const Point& target) {
  const float yaw=std::atan2(target[2]-body.position[2],target[0]-body.position[0])*180.f/pi;
  return std::remainder(yaw-body.yawDegrees,360.f);
  }
void steer(NpcKeys::KeyState& keys,const NavKeyTeacher::State& body,const Point& target,
           float moveAngle=28.f) {
  const float error=angle(body,target);
  // applyKeys is digital: the magnitude of turn is ignored. Half a 100-ms turn prevents
  // alternating left/right forever around the desired yaw, including the armed turn rate.
  const float deadband=std::max(1.f,body.turnSpeedDegrees*.05f);
  if(!body.climbing && std::abs(error)>deadband)
    keys.turn=error>0 ? -1.f : 1.f;
  if(std::abs(error)<=moveAngle)
    keys.forward=1;
  }
}

NavKeyTeacher::Decision NavKeyTeacher::decide(const State& body,const NavMesh::Path& path,
                                            const Point& goal) {
  Decision out;
  out.keys.walk=true;
  out.hint.nextCorner=finite(goal) ? goal : Point{};
  if(!finite(body.position) || !finite(body.velocity) || !finite(goal) ||
     !std::isfinite(body.yawDegrees) || !std::isfinite(body.turnSpeedDegrees) ||
     body.turnSpeedDegrees<=0) {
    out.replan=true;
    return out;
    }
  const bool airborne=body.inAir || body.falling || body.jumping || body.climbing;
  if(distance(body.position,goal)<1.f && !airborne) {
    out.arrived=true;
    return out;
    }
  if(!path.complete || path.corners.empty()) {
    out.replan=true;
    return out;
    }
  for(const auto& p:path.corners)
    if(!finite(p)) {
      out.replan=true;
      return out;
      }
  for(const auto& link:path.transitions)
    if(link.cornerIndex+1>=path.corners.size() || !finite(link.start) || !finite(link.end)) {
      out.replan=true;
      return out;
      }
  out.pathValid=true;

  // Match the body to its current segment rather than keeping a historical corner cursor.
  // Height participates in matching, so a bridge cannot match the ground beneath it.
  size_t corner=0;
  float best=distance(body.position,path.corners.front());
  for(size_t i=1;i<path.corners.size();++i) {
    bool linkSegment=false;
    for(const auto& link:path.transitions)
      linkSegment|=link.cornerIndex+1==i;
    if(linkSegment)
      continue;
    const auto& a=path.corners[i-1];
    const auto& b=path.corners[i];
    const float t=std::clamp(along(body.position,a,b),0.f,1.f);
    const float error=distance(body.position,lerp(a,b,t));
    if(error<best-1e-4f || (i==1 && error<=best+1e-4f)) {
      best=error;
      corner=i;
      }
    }

  // Never geometrically skip an untraversed link. The landing must be physically near the
  // far endpoint, or be past it on the far side at the landing's elevation. This also makes
  // decisions reconstructible in midair without a remembered "link started" flag.
  const NavMesh::Path::Transition* active=nullptr;
  for(const auto& link:path.transitions) {
    const float t=along(body.position,link.start,link.end);
    const bool landed=!airborne && body.supported &&
                      ((t>=.5f && close(body.position,link.end,.6f,.3f)) ||
                       (t>=1.f && std::abs(body.position[1]-link.end[1])<=.6f));
    if(landed)
      continue;
    if(link.cornerIndex<=corner ||
       (t>0.f && t<1.f && horizontal(body.position,lerp(link.start,link.end,t))<.8f)) {
      active=&link;
      corner=link.cornerIndex;
      break;
      }
    }

  if(!active) {
    const float speed=std::hypot(body.velocity[0],body.velocity[2]);
    const float radius=std::clamp(.18f+.04f*speed,.18f,.26f);
    while(corner+1<path.corners.size() && close(body.position,path.corners[corner],radius)) {
      bool takeoff=false;
      for(const auto& link:path.transitions)
        if(link.cornerIndex==corner) {active=&link;takeoff=true;break;}
      if(takeoff)
        break;
      ++corner;
      }
    }

  out.hint.cornerIndex=corner;
  out.hint.nextCorner=path.corners[corner];
  out.hint.remainingLength=remaining(body.position,path,corner);
  out.replan=best>1.5f && !airborne;

  if(active) {
    out.hint.transitionId=active->id;
    out.hint.transitionType=active->type;
    const float t=along(body.position,active->start,active->end);
    // A nearby point in the link's corridor is not its native takeoff. In
    // particular, running past a wall .35 m to one side never triggers a climb.
    // Walk back to the actual start until the narrow physical window is reached.
    const bool takeoff=close(body.position,active->start,.10f,.3f);
    const float alongVelocity=body.velocity[0]*(active->end[0]-active->start[0])+
                              body.velocity[2]*(active->end[2]-active->start[2]);
    // A moving drop reconstructs its departure from pose and velocity. It may
    // still be inside the takeoff window while the standing flag has closed.
    const bool advancingDrop=active->type==NavMesh::LinkType::Drop && t>0.f && alongVelocity>0.f &&
                             horizontal(body.position,lerp(active->start,active->end,t))<=.10f;
    const bool traversing=airborne || advancingDrop;
    if(takeoff || traversing) {
      out.hint.onTransition=true;
      out.hint.cornerIndex=active->cornerIndex+1;
      out.hint.nextCorner=active->end;
      out.hint.remainingLength=remaining(body.position,path,active->cornerIndex+1);
      out.keys.walk=false;
      steer(out.keys,body,active->end,18.f);
      const bool aligned=std::abs(angle(body,active->end))<=std::max(1.f,body.turnSpeedDegrees*.05f);
      // Native probing acts from rest at the takeoff. Run-mode idle stops the
      // preceding walk promptly; walk-mode idle can keep its root motion alive.
      // Once a drop is moving out along the link, do not stop it for standing.
      if(!airborne && !advancingDrop) {
        out.keys.forward=0;
      }
      // NpcKeys advances an airborne body before consulting jump; avoid retriggering jump
      // on landing by emitting it only at the takeoff while the body is supported.
      if(takeoff && body.supported && body.standing && !airborne && !body.sliding && !body.swimming &&
         aligned && !advancingDrop) {
        out.keys.walk=false;out.keys.forward=1;
        if(active->type==NavMesh::LinkType::Jump)
          out.keys.jump=true;
        else if(active->type==NavMesh::LinkType::Climb)
          out.keys.climb=true;
        }
      return out;
      }
    }

  steer(out.keys,body,out.hint.nextCorner);
  return out;
  }

NavKeyTeacher::Decision NavKeyTeacher::decideV2(const State& body,const NavMesh::Path& localPath,
                                              const Point& goal,const Point& carrot,
                                              bool directWalkable) {
  if(!finite(carrot)) {
    Decision out;
    out.keys.walk=true;
    out.replan=true;
    return out;
    }
  Decision out;
  if(directWalkable) {
    NavMesh::Path direct;
    direct.complete=true;
    direct.corners={body.position,carrot};
    // The synthetic ground segment has no Detour corner that could change its aim.
    out=decide(body,direct,goal);
    } else {
    out=decide(body,localPath,goal);
    }
  if(out.pathValid && !out.hint.onTransition && !body.climbing) {
    const float error=angle(body,out.hint.nextCorner);
    const float deadband=std::max(10.f,body.turnSpeedDegrees*.05f);
    out.keys.turn=std::abs(error)>deadband ? (error>0 ? -1.f : 1.f) : 0.f;
    }
  return out;
  }

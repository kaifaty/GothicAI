#include "followtrail.h"
#include "navigationpassage.h"

#include <algorithm>
#include <cmath>

using Tempest::Vec3;

namespace {
constexpr float Pi = 3.14159265358979323846f;
}

const char* FollowTrail::name(Source source) {
  switch(source) {
    case Source::Hold: return "hold";
    case Source::Direct: return "direct";
    case Source::Cut: return "cut";
    case Source::Trail: return "trail";
    case Source::Line: return "line";
    case Source::Unseen: return "unseen";
    case Source::Heading: return "heading";
    case Source::Aside: return "aside";
    case Source::LastSeen: return "last_seen";
    case Source::Count: break;
    }
  return "invalid";
  }

void FollowTrail::reset() {
  trail.clear();progress=0;visibleNow=false;lostAtEnd=false;
  }

void FollowTrail::observe(const Vec3& object, bool visible) {
  visibleNow=visible;
  if(!visible)
    return;
  lostAtEnd=false;
  if(trail.empty()) {
    trail.push_back({object,0});
    progress=0;
    return;
    }
  const float step=(object-trail.back().position).length();
  if(step>=spacingCm)
    trail.push_back({object,trail.back().arcCm+double(step)});
  // Short memory: keep only the newest memoryCm of the trail.
  while(trail.size()>=2 && trail.back().arcCm-trail[1].arcCm>=double(memoryCm))
    trail.pop_front();
  progress=std::max(progress,trail.front().arcCm);
  }

void FollowTrail::advance(const Vec3& follower) {
  if(trail.empty())
    return;
  double best=double((follower-trail.front().position).length()),along=trail.front().arcCm;
  for(size_t i=1;i<trail.size();++i) {
    const auto a=trail[i-1].position,delta=trail[i].position-a;
    const float square=Vec3::dotProduct(delta,delta);
    const float t=square>0 ? std::clamp(Vec3::dotProduct(follower-a,delta)/square,0.f,1.f) : 0.f;
    const double distance=double((a+delta*t-follower).length());
    if(distance<best) {best=distance;along=trail[i-1].arcCm+double(t)*(trail[i].arcCm-trail[i-1].arcCm);}
    }
  // Progress never decreases, as on a road. Points already passed are forgotten.
  progress=std::max(progress,along);
  while(trail.size()>=2 && trail[1].arcCm<=progress)
    trail.pop_front();
  }

Vec3 FollowTrail::at(double arcCm) const {
  if(arcCm<=trail.front().arcCm)
    return trail.front().position;
  for(size_t i=1;i<trail.size();++i) {
    if(arcCm>trail[i].arcCm)
      continue;
    const double length=trail[i].arcCm-trail[i-1].arcCm;
    const float t=length>0 ? float((arcCm-trail[i-1].arcCm)/length) : 1.f;
    return trail[i-1].position+(trail[i].position-trail[i-1].position)*t;
    }
  return trail.back().position;
  }

double FollowTrail::standoffArc(const Vec3& object) const {
  // Walk forward from the follower's projection: the carrot may go only as far
  // as the trail stays standoffCm from the object. A loop of the trail that
  // passes the object earlier ends the usable stretch there, not at its end.
  const auto outside=[&](const Vec3& p) {return (p-object).length()>=standoffCm;};
  Vec3 from=at(progress);
  double fromArc=progress;
  if(!outside(from))
    return progress;
  for(size_t i=1;i<trail.size();++i) {
    if(trail[i].arcCm<=progress)
      continue;
    const auto delta=trail[i].position-from;
    const float square=Vec3::dotProduct(delta,delta);
    // Distance to the object along a straight segment is smallest at the
    // object's projection; if even that point is outside, so is the segment.
    const float nearest=square>0 ? std::clamp(Vec3::dotProduct(object-from,delta)/square,0.f,1.f) : 0.f;
    if(!outside(from+delta*nearest)) {
      float low=0,high=nearest;
      for(int k=0;k<24;++k) {
        const float middle=(low+high)*.5f;
        if(outside(from+delta*middle)) low=middle;
        else high=middle;
        }
      return fromArc+double(low)*(trail[i].arcCm-fromArc);
      }
    from=trail[i].position;fromArc=trail[i].arcCm;
    }
  return trail.back().arcCm;
  }

double FollowTrail::lastStandoffArc(const Vec3& object) const {
  // The first rule of this module: the newest trail point standoffCm away.
  // Kept only to count the decisions the forward rule changes.
  const auto outside=[&](const Vec3& p) {return (p-object).length()>=standoffCm;};
  if(outside(trail.back().position))
    return trail.back().arcCm;
  for(size_t i=trail.size()-1;i-->0;) {
    if(!outside(trail[i].position))
      continue;
    float low=0,high=1;
    const auto a=trail[i].position,delta=trail[i+1].position-a;
    for(int k=0;k<24;++k) {
      const float middle=(low+high)*.5f;
      if(outside(a+delta*middle)) low=middle;
      else high=middle;
      }
    return trail[i].arcCm+double(low)*(trail[i+1].arcCm-trail[i].arcCm);
    }
  return -1;
  }

Vec3 FollowTrail::clearAlong(const NavigationMemory::View& memory, const Vec3& from, const Vec3& to) {
  // The farthest point of from->to that crosses no static hazard the follower has seen.
  if(nearClear(memory,from,to))
    return to;
  const float length=(to-from).length();
  for(float along=length-spacingCm;along>footprintCm;along-=spacingCm) {
    const auto point=from+(to-from)*(along/length);
    if(nearClear(memory,from,point))
      return point;
    }
  return from;
  }

FollowTrail::Result FollowTrail::unseen(const Vec3& follower, const Vec3& object, const Vec3& direct,
                                        const NavigationMemory::View& memory) const {
  // The companion still hears where the object is: the straight carrot toward it,
  // but never through a static hazard it has seen (unknown ground does not close).
  Result result;
  if(nearClear(memory,follower,direct)) {
    result.carrot=direct;result.source=Source::Unseen;
    return result;
    }
  // Around the seen hazard by hearing: turn the direction toward the object by
  // 15-degree steps to either side, up to 90 degrees, and take the first open one.
  const auto aside=[&](Vec3& carrot) {
    auto toward=direct-follower;toward.y=0;
    const float reach=std::min(lookaheadCm,toward.length());
    if(reach<=footprintCm)
      return false;
    const float base=std::atan2(toward.z,toward.x);
    for(int step=1;step<=6;++step)
      for(const float side:{1.f,-1.f}) {
        const float angle=base+side*float(step)*Pi/12.f;
        const auto point=follower+Vec3(std::cos(angle),0,std::sin(angle))*reach;
        if((point-object).length()>=standoffCm && nearClear(memory,follower,point)) {
          carrot=point;
          return true;
          }
        }
    return false;
    };
  if(trail.empty()) {
    // Never seen: around the hazard; only if all is closed, stop before it.
    result.source=Source::Unseen;result.detour=true;
    if(!aside(result.carrot))
      result.carrot=clearAlong(memory,follower,direct);
    return result;
    }
  // Lost at the last seen point: where the object was last seen heading.
  if(trail.size()>=2) {
    auto heading=trail.back().position-trail[trail.size()-2].position;
    heading.y=0;
    const float length=heading.length();
    if(length>0) {
      const auto ahead=trail.back().position+heading*(lookaheadCm/length);
      if((ahead-object).length()>=standoffCm && nearClear(memory,follower,ahead)) {
        result.carrot=ahead;result.source=Source::Heading;
        return result;
        }
      }
    }
  // Then around the hazard by hearing, as for an object never seen; the follower
  // stands at the last seen point only if every direction is closed.
  if(lostAside && aside(result.carrot)) {
    result.source=Source::Aside;
    return result;
    }
  result.carrot=trail.back().position;result.source=Source::LastSeen;
  return result;
  }

FollowTrail::Result FollowTrail::guide(const Vec3& follower, const Vec3& object,
                                       const NavigationMemory::View& memory) {
  advance(follower);
  Result result;
  const auto delta=object-follower;
  const float distance=delta.length();
  if(distance<=standoffCm) {
    result.carrot=follower;result.source=Source::Hold;
    return result;
    }
  const Vec3 direct=follower+delta*((distance-standoffCm)/distance);
  if(nearFree(memory,follower,direct)) {
    result.carrot=direct;result.source=Source::Direct;
    return result;
    }
  // At the last seen point without sight of the object the trail says nothing
  // more; the object is followed by hearing until it is seen again.
  if(!trail.empty() && !visibleNow && (follower-trail.back().position).length()<=reachedCm)
    lostAtEnd=true;
  if(trail.empty() || lostAtEnd)
    return unseen(follower,object,direct,memory);
  const double standoff=standoffArc(object),firstRule=lastStandoffArc(object);
  result.loopLimited=std::abs(firstRule-standoff)>1.;
  if(standoff<=progress) {
    result.carrot=direct;result.source=Source::Line;result.loopLine=firstRule>progress;
    return result;
    }
  const double ahead=std::min(progress+double(lookaheadCm),standoff);
  if(standoff>ahead) {
    // Cut beyond the lookahead only along a passage the near memory sees free.
    const auto farthest=at(standoff);
    if(nearFree(memory,follower,farthest)) {
      result.carrot=farthest;result.source=Source::Cut;
      return checked(result,object,direct);
      }
    for(size_t i=trail.size();i-->0;) {
      if(trail[i].arcCm>=standoff)
        continue;
      if(trail[i].arcCm<=ahead)
        break;
      if(nearFree(memory,follower,trail[i].position)) {
        result.carrot=trail[i].position;result.source=Source::Cut;
        return checked(result,object,direct);
        }
      }
    }
  // The lookahead point on the trail. Where the near memory has already seen a
  // hazard between the follower and it, step back along the trail toward the
  // follower, but never nearer than pullbackMinCm: a carrot at the follower's feet
  // would hold it while the object walks away. If every point that far is
  // blocked, the nearest of them remains and the planner routes around.
  result.source=Source::Trail;
  result.carrot=at(ahead);
  if(nearClear(memory,follower,result.carrot))
    return checked(result,object,direct);
  for(size_t i=trail.size();i-->0;) {
    if(trail[i].arcCm>=ahead)
      continue;
    if(trail[i].arcCm<=progress || (trail[i].position-follower).length()<pullbackMinCm)
      break;
    result.carrot=trail[i].position;
    if(nearClear(memory,follower,result.carrot))
      return checked(result,object,direct);
    }
  return checked(result,object,direct);
  }

FollowTrail::Result FollowTrail::checked(Result result, const Vec3& object, const Vec3& direct) {
  // Every trail and cut carrot lies on the trail before its first entry into the
  // standoff circle. The last guard keeps the invariant against rounding.
  if((result.carrot-object).length()<standoffCm-.5f) {
    result.carrot=direct;result.source=Source::Line;result.guarded=true;
    }
  return result;
  }

bool FollowTrail::nearFree(const NavigationMemory::View& memory, const Vec3& from, const Vec3& to) {
  return passage(memory,from,to,true);
  }

bool FollowTrail::nearClear(const NavigationMemory::View& memory, const Vec3& from, const Vec3& to) {
  return passage(memory,from,to,false);
  }

bool FollowTrail::passage(const NavigationMemory::View& memory, const Vec3& from, const Vec3& to, bool seen) {
  // The near-map check is shared with familiar-road guidance (game/navigationpassage.h).
  return NavigationPassage::near(memory,from,to,seen,corridorCm,footprintCm);
  }

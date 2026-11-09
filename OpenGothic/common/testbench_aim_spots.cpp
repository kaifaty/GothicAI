#include "testbench_internal.h"

#include <algorithm>
#include <cmath>
#include <map>

#include "game/gamescript.h"
#include "world/objects/npc.h"
#include "world/world.h"

using namespace Tempest;
using namespace testbench_detail;

namespace {

// Reuse the hero's skeleton prediction at a candidate perch without teleporting him during a search.
Vec3 perchHead(const Npc& pl, const Vec3& origin, const Vec3& forward, const Vec3& target, float angle) {
  const Vec3 f = facing(pl), s(-f.z,0,f.x), side(-forward.z,0,forward.x);
  const Vec3 d = target-origin;
  const Vec3 mapped = pl.position() + f*Vec3::dotProduct(d,forward) + s*Vec3::dotProduct(d,side) + Vec3(0,d.y,0);
  const Vec3 head = pl.aimHead(mapped,angle)-pl.position();
  return origin + forward*Vec3::dotProduct(head,f) + side*Vec3::dotProduct(head,s) + Vec3(0,head.y,0);
  }

bool perchLean(World& w, const Npc& pl, const Vec3& origin, const Vec3& forward, const Vec3& target,
               float minDown, float maxDown, float& down, float& bend) {
  const Vec3 head = perchHead(pl,origin,forward,target,0.f);
  const Vec3 d = target-head;
  down = -std::atan2(d.y,std::sqrt(d.x*d.x+d.z*d.z))*180.f/float(M_PI);
  const auto blocked = w.physic()->ray(head,target);
  if(down<minDown || down>maxDown || !blocked.hasCol || (blocked.v-head).length()>200.f)
    return false;
  Vec3 previous = head;
  for(float angle=1.f; angle<=Aim::LeanMax; angle+=1.f) {
    const Vec3 next = perchHead(pl,origin,forward,target,angle*float(M_PI)/180.f);
    if(w.physic()->ray(previous,next).hasCol)
      return false;
    if(!w.physic()->ray(next,target).hasCol) {
      bend = angle;
      return true;
      }
    previous = next;
    }
  return false;
  }

}

namespace testbench_detail {

bool aimSpotsCommand(bool& hasPrev,const std::string& line,const std::vector<std::string>& a,World& w,Npc& pl) {
  const auto& c=a[0];
  if(c=="cansee") {
    // cansee <npc|nearest> [edge yaw]: visibility and the head/arrow reach toward the edge, in cm from the feet
    Npc* n = findNpc(w,pl,a.size()>1 ? a[1] : "nearest");
    if(n==nullptr || n==&pl) {
      say("cansee: no npc " + line);
      return true;
      }
    const Vec3  tg   = n->collosionCenter();
    const Vec3  head = pl.aimHead(tg,0.f);
    const auto  lean = pl.aimLean(pl.hasManualAim() ? pl.manualAimPoint() : tg,pl.hasManualAim());
    const Vec3  d    = tg-head;
    const float h    = std::sqrt(d.x*d.x+d.z*d.z);
    auto        r    = w.physic()->ray(head,tg);
    const auto& fr = w.script().focusRange();
    char buf[800];
    std::snprintf(buf,sizeof(buf),"cansee %s: head %d, over the edge %d; dist %.0f, down %.0f deg; the ray from the head hits at %.0f cm of %.0f; bow focus %.0f..%.0f",
                  symName(w,n).c_str(),int(!r.hasCol),int(pl.canSeeNpcOverEdge(*n)),double(d.length()),
                  double(-std::atan2(d.y,h)*180.f/float(M_PI)),r.hasCol ? double((r.v-head).length()) : -1.0,double(d.length()),
                  double(fr.npc_range1),double(fr.npc_range2));
    say(buf);
    const Vec3  dir  = Vec3::normalize(Vec3(d.x,0,d.z));
    const Vec3  eye  = pl.aimHead(tg,lean.angle);
    const Vec3  full = pl.aimHead(tg,Aim::LeanMax*float(M_PI)/180.f);
    auto        r2   = w.physic()->ray(eye,tg);
    const float degrees = lean.angle*180.f/float(M_PI);
    const float before = std::max(0.f,degrees-0.25f);
    const Vec3  prior = pl.aimHead(tg,before*float(M_PI)/180.f);
    const bool  priorSeen = !w.physic()->ray(prior,tg).hasCol;
    std::snprintf(buf,sizeof(buf),"  leaned head: required_deg %.2f current_deg %.2f line %s visible %d room %d edge %d hit_cm %.2f shot_clear %d; to it %d (hit at %.0f of %.0f), before %.2f deg seen %d; max_forward_cm %.1f from feet, head at %.0f above the feet",
                  double(degrees),double(Aim::playerProbe.lean),leanLineName(lean.line),int(lean.visible),int(lean.room),
                  int(lean.edge),double(lean.hitDistance),int(lean.shotClear),
                  int(!r2.hasCol),r2.hasCol ? double((r2.v-eye).length()) : -1.0,double((tg-eye).length()),double(before),int(priorSeen),
                  double(Vec3::dotProduct(full-pl.position(),dir)),double(eye.y-pl.position().y));
    say(buf);
    const Vec3 actual = pl.mapHeadBone();
    const Vec3 predicted = pl.aimCurrentHead();
    std::snprintf(buf,sizeof(buf),"  actual head: visible %d prediction_error_cm %.2f",
                  int(!w.physic()->ray(actual,tg).hasCol),double((actual-predicted).length()));
    say(buf);
    const float yaw = num(a,2,pl.rotation());
    const float rad = yaw*float(M_PI)/180.f;
    const Vec3 edgeDir(std::cos(rad),0,std::sin(rad));
    Vec3 arrow = {}, arrowDir = {};
    const bool hasArrow = pl.arrowRay(arrow,arrowDir);
    const Vec3 fullArrow = pl.aimArrow(tg,Aim::LeanMax*float(M_PI)/180.f);
    std::snprintf(buf,sizeof(buf),"  edge reach yaw %.1f: current head_forward_cm %.2f arrow_forward_cm %.2f arrow_valid %d head (%.2f %.2f %.2f) arrow (%.2f %.2f %.2f); full70 head_forward_cm %.2f arrow_forward_cm %.2f head (%.2f %.2f %.2f) arrow (%.2f %.2f %.2f) feet (%.2f %.2f %.2f)",
                  double(yaw),double(Vec3::dotProduct(actual-pl.position(),edgeDir)),
                  hasArrow ? double(Vec3::dotProduct(arrow-pl.position(),edgeDir)) : -1.0,int(hasArrow),
                  double(actual.x),double(actual.y),double(actual.z),double(arrow.x),double(arrow.y),double(arrow.z),
                  double(Vec3::dotProduct(full-pl.position(),edgeDir)),double(Vec3::dotProduct(fullArrow-pl.position(),edgeDir)),
                  double(full.x),double(full.y),double(full.z),double(fullArrow.x),double(fullArrow.y),double(fullArrow.z),
                  double(pl.position().x),double(pl.position().y),double(pl.position().z));
    say(buf);
    std::snprintf(buf,sizeof(buf),"  directional lean: edge_cm %.2f drop_cm %.2f support_y %.2f head_clear %d shot_clear %d head_hit_cm %.2f shot_hit_cm %.2f limited %d room_limited %d",
                  double(lean.edgeDistance),double(std::isfinite(lean.edgeDrop) ? lean.edgeDrop : -1.f),double(lean.edgeHeight),int(lean.headClear),int(lean.shotClear),
                  double(lean.headHitDistance),double(lean.shotHitDistance),int(lean.limited),int(lean.roomLimited));
    say(buf);
    if(hasArrow && pl.hasManualAim()) {
      const Vec3 look = Vec3::normalize(pl.aimLook());
      const Vec3 lookFlat = Vec3::normalize(Vec3(look.x,0,look.z));
      const float edgeProjection = lean.edgeDistance*Vec3::dotProduct(lookFlat,edgeDir);
      const auto headRay = w.physic()->ray(actual,actual+look*200.f);
      const auto shotRay = w.physic()->ray(arrow,arrow+Vec3::normalize(arrowDir)*200.f);
      std::snprintf(buf,sizeof(buf),"  actual edge rays: head_over_edge_cm %.2f arrow_over_edge_cm %.2f head_hit_cm %.2f at (%.2f %.2f %.2f) arrow_hit_cm %.2f at (%.2f %.2f %.2f)",
                    double(Vec3::dotProduct(actual-pl.position(),edgeDir)-edgeProjection),
                    double(Vec3::dotProduct(arrow-pl.position(),edgeDir)-edgeProjection),
                    headRay.hasCol ? double((headRay.v-actual).length()) : -1.0,
                    double(headRay.v.x),double(headRay.v.y),double(headRay.v.z),
                    shotRay.hasCol ? double((shotRay.v-arrow).length()) : -1.0,
                    double(shotRay.v.x),double(shotRay.v.y),double(shotRay.v.z));
      say(buf);
      }
    }
  else if(c=="slopes" || c=="stairs") {
    for(auto& s:findSpots(w,pl,c=="stairs" ? K_Stairs : K_Slope,num(a,1,10),num(a,2,35),"",30)) {
      char buf[256];
      std::snprintf(buf,sizeof(buf),"%s %s angle=%.1f rough=%.1f dist=%.0f",c.c_str(),s.name.c_str(),s.angle,s.rough,s.dist);
      say(buf);
      }
    }
  else if(c=="ledges") {
    // ledges [min drop] [max drop] [count] [steep]: waypoints at an edge, open ground that much lower
    // 1.5..14 m away and seen from there (a target below); prints the waypoint, the yaw to face
    // (`face <yaw>`), the distance, the drop and the angle down; near first, or the steepest first
    const float minDrop = num(a,1,300), maxDrop = num(a,2,1000);
    const size_t count  = size_t(num(a,3,15));
    struct Ledge { std::string name; float yaw, dist, drop, down, from; };
    std::vector<Ledge> all;
    const Vec3 at = pl.position();
    w.findWayPoint(at,[&](const WayPoint& wp) {
      const float g0 = groundY(w,wp.pos.x,wp.pos.z,wp.pos.y);
      if(std::isnan(g0))
        return false;
      float an = 0, rough = 0;
      Vec3  dn;
      if(!slopeAt(w,Vec3(wp.pos.x,g0,wp.pos.z),an,dn,rough) || an>15)
        return false;
      if(w.physic()->ray({wp.pos.x,g0+200,wp.pos.z},{wp.pos.x,g0+1500,wp.pos.z}).hasCol)
        return false; // a roof above: indoors
      Ledge best;
      best.down = 0;
      for(int k=0; k<16; ++k) {
        const float y = float(k)*22.5f*float(M_PI)/180.f;
        for(float d:{150.f,250.f,400.f,550.f,700.f,1000.f,1400.f}) {
          const Vec3 p = Vec3(wp.pos.x+std::cos(y)*d, g0, wp.pos.z+std::sin(y)*d);
          auto r = w.physic()->ray({p.x,g0+200,p.z},{p.x,g0-maxDrop-100,p.z});
          if(!r.hasCol)
            continue;
          const float drop = g0-r.v.y;
          if(drop<minDrop || drop>maxDrop)
            continue;
          const Vec3 eye = Vec3(wp.pos.x,g0+170,wp.pos.z), tg = r.v+Vec3(0,60,0);
          auto los = w.physic()->ray(eye,tg);
          if(los.hasCol && (los.v-tg).length()>40)
            continue;
          const float down = std::atan2(drop,d)*180.f/float(M_PI);
          if(down>best.down)
            best = Ledge{wp.name, float(k)*22.5f, d, drop, down, (wp.pos-at).length()};
          }
        }
      if(best.down>0)
        all.push_back(best);
      return false;
      });
    if(a.size()>4 && a[4]=="steep")
      std::sort(all.begin(),all.end(),[](const Ledge& x, const Ledge& y) { return x.down>y.down; }); else
      std::sort(all.begin(),all.end(),[](const Ledge& x, const Ledge& y) { return x.from<y.from; });
    say("ledges " + std::to_string(all.size()));
    for(size_t i=0; i<all.size() && i<count; ++i) {
      char buf[256];
      std::snprintf(buf,sizeof(buf),"ledge %s yaw=%.1f dist=%.0f drop=%.0f down=%.0f from=%.0f",all[i].name.c_str(),
                    double(all[i].yaw),double(all[i].dist),double(all[i].drop),double(all[i].down),double(all[i].from));
      say(buf);
      }
    }
  else if(c=="corner") {
    // corner <left|right> [count] | corner <left|right> go <i>: spots at a corner of a wall (near waypoints):
    // a wall right in front of the head, open far ahead leaning out to that side (Aim::SideReach): where
    // the manual aim leans out round it. Lists them (nearest first) or puts the hero at the i-th, facing
    // along it
    const bool  right = a.size()>1 && a[1]=="right";
    const bool  go    = a.size()>2 && a[2]=="go";
    const size_t pick = size_t(num(a,3,0));
    const size_t count = size_t(num(a,2,10));
    struct Spot { std::string name; Vec3 at; float yaw, front, from; };
    std::vector<Spot> all;
    const Vec3 at0 = pl.position();
    w.findWayPoint(at0,[&](const WayPoint& wp) {
      if((wp.pos-at0).length()>8000.f)
        return false;
      for(int k=0; k<16; ++k) {
        const float y    = float(k)*22.5f*float(M_PI)/180.f;
        const Vec3  f    = Vec3(std::cos(y),0,std::sin(y));
        Vec3        sd   = Vec3::crossProduct(Vec3(0,1,0),f);
        sd = Vec3::normalize(sd)*(right ? 1.f : -1.f);
        for(float shift=-90; shift<=90; shift+=15) {
          const Vec3  p0 = Vec3(wp.pos.x,wp.pos.y+50,wp.pos.z);
          const Vec3  p1 = p0 + sd*shift;
          if(w.physic()->ray(p0,p1).hasCol)
            continue;
          const float g  = groundY(w,p1.x,p1.z,p1.y+50);
          if(std::isnan(g) || std::abs(g-wp.pos.y)>40)
            continue;
          const Vec3 head = Vec3(p1.x,g+148,p1.z);
          auto       fr   = w.physic()->ray(head,head+f*400.f);
          if(!fr.hasCol || (fr.v-head).length()<50.f)
            continue; // no wall in front, or right at the face
          const Vec3 out = head + sd*Aim::SideReach;
          if(w.physic()->ray(head,out+sd*15.f).hasCol)
            continue; // no room to lean out
          if(w.physic()->ray(out,out+f*600.f).hasCol)
            continue; // walled leaning out too
          if(w.physic()->ray(Vec3(p1.x,g+60,p1.z),Vec3(p1.x,g+60,p1.z)+f*60.f).hasCol)
            continue; // a low step in front: not a corner
          all.push_back(Spot{wp.name,Vec3(p1.x,g,p1.z),float(k)*22.5f,(fr.v-head).length(),(p1-at0).length()});
          break;
          }
        }
      return false;
      });
    std::sort(all.begin(),all.end(),[](const Spot& x, const Spot& y) { return x.from<y.from; });
    if(go) {
      if(pick>=all.size()) {
        say("corner: no spot " + std::to_string(pick));
        return true;
        }
      auto& sp = all[pick];
      pl.setPosition(sp.at);
      face(pl,Vec3(std::cos(sp.yaw*float(M_PI)/180.f),0,std::sin(sp.yaw*float(M_PI)/180.f)));
      hasPrev = false;
      char buf[200];
      std::snprintf(buf,sizeof(buf),"corner %s %zu: %s yaw=%.1f wall in front %.0f cm",right ? "right" : "left",pick,sp.name.c_str(),
                    double(sp.yaw),double(sp.front));
      say(buf);
      return true;
      }
    say("corners " + std::to_string(all.size()));
    for(size_t i=0; i<all.size() && i<count; ++i) {
      char buf[200];
      std::snprintf(buf,sizeof(buf),"corner %zu %s yaw=%.1f front=%.0f from=%.0f",i,all[i].name.c_str(),double(all[i].yaw),
                    double(all[i].front),double(all[i].from));
      say(buf);
      }
    }
  else if(c=="walls") {
    // walls [count]: spots at a wall's edge (from a waypoint, walking to where the ground falls away) with
    // the foot of the wall 2.5..10 m below, 1 m out: hidden from the head, seen leaning over (the aim's
    // lean); prints the waypoint and the yaw for `goto`, `edge <yaw>`
    const size_t count = size_t(num(a,1,15));
    struct Wall { std::string name; float yaw, drop, from, down, bend; };
    std::vector<Wall> all;
    const Vec3 at = pl.position();
    auto ground = [&](const Vec3& p, float y0, float up, float down) {
      auto r = w.physic()->ray({p.x,y0+up,p.z},{p.x,y0-down,p.z});
      return r.hasCol ? r.v.y : NAN;
      };
    w.findWayPoint(at,[&](const WayPoint& wp) {
      const float g0 = groundY(w,wp.pos.x,wp.pos.z,wp.pos.y);
      if(std::isnan(g0))
        return false;
      for(int k=0; k<16; ++k) {
        const float y   = float(k)*22.5f*float(M_PI)/180.f;
        const Vec3  dir = Vec3(std::cos(y),0,std::sin(y));
        Vec3 e = Vec3(wp.pos.x,g0,wp.pos.z);
        bool edge = false;
        for(float d=25; d<=600; d+=25) {
          const Vec3  p = Vec3(wp.pos.x,g0,wp.pos.z) + dir*d;
          const float g = ground(p,e.y,100,150);
          if(std::isnan(g) || std::abs(g-e.y)>60) {
            edge = true;
            break;
            }
          e = Vec3(p.x,g,p.z);
          }
        if(!edge)
          continue;
        const Vec3  st   = e - dir*35.f;
        const Vec3  t    = st + dir*135.f;
        const float tg   = ground(t,st.y,-60,1000);
        if(std::isnan(tg) || st.y-tg<250 || st.y-tg>1000)
          continue;
        const Vec3 aimT = Vec3(t.x,tg+60,t.z);
        float down = 0, bend = 0;
        if(!perchLean(w,pl,st,dir,aimT,Aim::LeanStart,90.f,down,bend))
          continue;
        if(w.physic()->ray({t.x,tg+100,t.z},{t.x,tg+1500,t.z}).hasCol)
          continue; // under a roof, an arch
        all.push_back(Wall{wp.name, float(k)*22.5f, st.y-tg, (wp.pos-at).length(),down,bend});
        break;
        }
      return false;
      });
    std::sort(all.begin(),all.end(),[](const Wall& x, const Wall& y) { return x.from<y.from; });
    say("walls " + std::to_string(all.size()));
    for(size_t i=0; i<all.size() && i<count; ++i) {
      char buf[256];
      std::snprintf(buf,sizeof(buf),"wall %s yaw=%.1f drop=%.0f from=%.0f down=%.1f bend=%.0f",all[i].name.c_str(),double(all[i].yaw),
                    double(all[i].drop),double(all[i].from),double(all[i].down),double(all[i].bend));
      say(buf);
      }
    }
  else if(c=="perches") {
    // perches [radius 3000] [count 15] [min drop 250] [max drop 1000] [min down 30] [max down 90]
    //         [target dist 135] [edge margin 35]: like `walls`, but on any top around the
    // hero, waypoint or not (a roof, a rock, a wall's top): a grid of the highest surfaces, each one you can stand
    // on (slope up to 35 deg, 1.9 m free above) at an edge with the foot 1 m out hidden from the head, seen
    // leaning over, the sky above it; one a 2 m cell, the nearest first: `goto pos <x y z>`, `face <yaw>`.
    // A farther target and larger margin can find blocked 40..55 degree shots instead of steep wall-foot shots.
    const float  radius = num(a,1,3000);
    const size_t count  = size_t(num(a,2,15));
    const float  minD   = num(a,3,250), maxD = num(a,4,1000);
    const float  minDown = num(a,5,Aim::LeanStart), maxDown = num(a,6,90);
    const float  targetDist = num(a,7,135), margin = num(a,8,35);
    const Vec3   at     = pl.position();
    auto ground = [&](const Vec3& p, float y0, float up, float down) {
      auto r = w.physic()->ray({p.x,y0+up,p.z},{p.x,y0-down,p.z});
      return r.hasCol ? r.v.y : NAN;
      };
    struct Perch { Vec3 st; float yaw, drop, slope, from, down, bend; };
    std::map<std::pair<int,int>,Perch> cells;
    for(float x=-radius; x<=radius; x+=50)
      for(float z=-radius; z<=radius; z+=50) {
        if(x*x+z*z>radius*radius)
          continue;
        const Vec3 top = Vec3(at.x+x,at.y,at.z+z);
        const float s  = ground(top,at.y,3000,6000);
        if(std::isnan(s))
          continue;
        const Vec3 sp = Vec3(top.x,s,top.z);
        if(w.physic()->ray(sp+Vec3(0,5,0),sp+Vec3(0,190,0)).hasCol)
          continue;
        float slope = 0, rough = 0;
        Vec3  dn;
        if(!slopeAt(w,sp,slope,dn,rough) || slope>35 || rough>20)
          continue;
        for(int k=0; k<16; ++k) {
          const float y   = float(k)*22.5f*float(M_PI)/180.f;
          const Vec3  dir = Vec3(std::cos(y),0,std::sin(y));
          Vec3 e = sp;
          bool edge = false;
          for(float d=25; d<=300; d+=25) {
            const Vec3  p = sp + dir*d;
            const float g = ground(p,e.y,100,150);
            if(std::isnan(g) || std::abs(g-e.y)>60) {
              edge = true;
              break;
              }
            e = Vec3(p.x,g,p.z);
            }
          if(!edge)
            continue;
          const Vec3  st = e - dir*margin;
          const float sg = ground(st,e.y,60,100);
          if(std::isnan(sg))
            continue;
          const Vec3  stand = Vec3(st.x,sg,st.z);
          const Vec3  t     = stand + dir*targetDist;
          const float tg    = ground(t,stand.y,-60,maxD+100);
          if(std::isnan(tg) || stand.y-tg<minD || stand.y-tg>maxD)
            continue;
          const Vec3 aimT = Vec3(t.x,tg+60,t.z);
          float down = 0, bend = 0;
          if(!perchLean(w,pl,stand,dir,aimT,minDown,maxDown,down,bend))
            continue;
          if(w.physic()->ray({t.x,tg+100,t.z},{t.x,tg+1500,t.z}).hasCol)
            continue; // under a roof, an arch
          const auto key  = std::make_pair(int(std::floor(stand.x/200.f)),int(std::floor(stand.z/200.f)));
          const Perch pc  = {stand, float(k)*22.5f, stand.y-tg, slope, (stand-at).length(),down,bend};
          auto        it  = cells.find(key);
          if(it==cells.end() || pc.slope<it->second.slope)
            cells[key] = pc;
          break;
          }
        }
    std::vector<Perch> all;
    for(auto& [k,v]:cells)
      all.push_back(v);
    std::sort(all.begin(),all.end(),[](const Perch& x, const Perch& y) { return x.from<y.from; });
    say("perches " + std::to_string(all.size()));
    for(size_t i=0; i<all.size() && i<count; ++i) {
      char buf[320];
      std::snprintf(buf,sizeof(buf),"perch goto pos %.0f %.0f %.0f, face %.1f: drop=%.0f slope=%.0f above the hero %.0f, from=%.0f down=%.1f bend=%.0f; pin WOLF %.0f 0 %.0f",
                    double(all[i].st.x),double(all[i].st.y+5),double(all[i].st.z),double(all[i].yaw),double(all[i].drop),
                    double(all[i].slope),double(all[i].st.y-at.y),double(all[i].from),double(all[i].down),double(all[i].bend),
                    double(targetDist),double(-all[i].drop));
      say(buf);
      }
    }
  else if(c=="edge") {
    // edge <yaw> [max cm] [margin cm] [exact]: the hero is put that way to where the ground falls away, and stands
    // that far from the edge (35: closer the body slips over it) facing it; prints how deep it goes down
    // right past the edge. Optional exact refines the usual 25 cm search before applying the margin.
    const float y   = num(a,1,0)*float(M_PI)/180.f;
    const Vec3  dir = Vec3(std::cos(y),0,std::sin(y));
    const float max = num(a,2,2000);
    Vec3        pos = pl.position();
    const float g0  = pos.y;
    bool        found = false;
    float       boundary = 0;
    for(float d=25; d<=max; d+=25) {
      const Vec3  p = pl.position() + dir*d;
      auto        r = w.physic()->ray({p.x,g0+100,p.z},{p.x,g0-150,p.z});
      if(!r.hasCol || std::abs(r.v.y-g0)>60) {
        found = true;
        boundary = d;
        break;
        }
      pos = Vec3(p.x,r.v.y,p.z);
      }
    if(!found) {
      say("edge: no edge that way");
      return true;
      }
    if(a.size()>4 && a[4]=="exact") {
      float lo = boundary-25.f, hi = boundary;
      for(int i=0; i<8; ++i) {
        const float mid = (lo+hi)*0.5f;
        const Vec3  p   = pl.position()+dir*mid;
        const auto  r   = w.physic()->ray({p.x,g0+100,p.z},{p.x,g0-150,p.z});
        if(r.hasCol && std::abs(r.v.y-g0)<=60) {
          lo = mid;
          pos = Vec3(p.x,r.v.y,p.z);
          } else {
          hi = mid;
          }
        }
      }
    // a step back from the very edge, the feet on the ground
    pos -= dir*num(a,3,35);
    pl.setPosition(pos);
    face(pl,dir);
    pl.updateTransform();
    const Vec3 past = pos + dir*80.f;
    auto       r    = w.physic()->ray({past.x,pos.y-60,past.z},{past.x,pos.y-3000,past.z});
    char buf[200];
    std::snprintf(buf,sizeof(buf),"edge at %.0f %.0f %.0f, down past it %.0f",pos.x,pos.y,pos.z,r.hasCol ? pos.y-r.v.y : -1.f);
    say(buf);
    }
  else if(c=="edgedist") {
    // edgedist <yaw> [max cm] [min drop cm]: measure the first drop ahead without moving the hero.
    // Follow the floor in 5 cm steps so a continuous slope does not count as an edge, then refine it.
    const float yaw     = num(a,1,0);
    const float y       = yaw*float(M_PI)/180.f;
    const Vec3  dir     = Vec3(std::cos(y),0,std::sin(y));
    const Vec3  at      = pl.position();
    const float max     = num(a,2,2000);
    const float minDrop = num(a,3,100);
    auto floor = [&](float d, float height) {
      const Vec3 p = at+dir*d;
      return w.physic()->ray({p.x,height+100,p.z},{p.x,height-3000,p.z});
      };
    auto r = floor(0,at.y);
    if(!r.hasCol) {
      say("edgedist: no floor under the hero");
      return true;
      }
    float lo = 0, height = r.v.y;
    for(float d=5; d<=max; d+=5) {
      r = floor(d,height);
      if(!r.hasCol || height-r.v.y>minDrop) {
        float hi = d;
        const float top = height;
        for(int i=0; i<8; ++i) {
          const float mid = (lo+hi)*0.5f;
          const auto  q   = floor(mid,top);
          if(q.hasCol && top-q.v.y<=minDrop)
            lo = mid; else
            hi = mid;
          }
        r = floor(hi,top);
        char buf[200];
        std::snprintf(buf,sizeof(buf),"edgedist yaw=%.1f distance=%.1f cm drop=%.1f cm (min=%.0f)",
                      double(yaw),double((lo+hi)*0.5f),double(r.hasCol ? top-r.v.y : -1.f),double(minDrop));
        say(buf);
        return true;
        }
      lo     = d;
      height = r.v.y;
      }
    say("edgedist: no edge within " + std::to_string(max) + " cm");
    }
  else return false;
  return true;
  }

}

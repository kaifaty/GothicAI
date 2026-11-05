#include "navmemoryteacher.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <queue>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace {
using Input = NavMemoryTeacher::Input;
using Cell = NavMemoryTeacher::Cell;
using Point = NavMemoryTeacher::Point;
using DiagnosticsOptions = NavMemoryTeacher::DiagnosticsOptions;
using Diagnostics = NavMemoryTeacher::Diagnostics;
using PlanStatus = NavMemoryTeacher::PlanStatus;
constexpr float Pi = 3.14159265358979323846f;
constexpr int NearSize = 64, FarSize = 100;
constexpr int NearCount = NearSize*NearSize, Count = NearCount+FarSize*FarSize;
constexpr float NearCell = 25, FarCell = 200, NearExtent = 800;
constexpr float Infinity = std::numeric_limits<float>::infinity();
constexpr float WaterMarginCm = 75.f;
// Body exit: the planning margin, the own track kept behind the feet, the
// longest exit, and NpcKeys' unarmed turn (90 deg/s) per 100-ms decision.
constexpr float FootprintCm = 15.f, TrackCm = 300.f, ExitCm = 300.f, TurnDegrees = 9.f;
constexpr size_t TrackPoints = 100;
// An escape step that leaves the body below the stuck speed for PushTicks decisions
// refuses its direction (a point RefusedCm along it) for RefusedTtl decisions.
constexpr float StuckSpeed = 18.f, RefusedCm = 30.f;
constexpr uint32_t PushTicks = 3, RefusedTtl = 50;
// An exit cell the body reached without finding a start there is not chosen again
// for FailedTtl decisions (within FailedCm of it).
constexpr float ReachedCm = 20.f, FailedCm = 25.f;
constexpr uint32_t FailedTtl = 100;

float length(Point p) { return std::hypot(p.rightCm,p.forwardCm); }
float distance(Point a,Point b) { return length({a.rightCm-b.rightCm,a.forwardCm-b.forwardCm}); }
float sign(float v) { return v<0 ? -1.f : 1.f; }
bool familiarDanger(const Cell& c) {
  for(size_t k=2;k<6;++k)
    if(c.channels[k]!=0) return true;
  return false;
  }
bool dangerous(const Cell& c) {
  // Movable evidence is always live, including when the static map is familiar.
  // Live static danger retains its hard veto even after confidence has faded.
  return c.channels[6]!=0 || (c.familiar==0 && familiarDanger(c));
  }
bool liveWater(const Cell& c) { return c.familiar==0 && c.channels[5]!=0; }
bool knownFree(const Cell& c,bool familiarMap) {
  if(c.familiar!=0 && !familiarMap) return false;
  return c.channels[0]>0 && c.channels[1]>0 && !familiarDanger(c) && !dangerous(c);
  }
float cost(const Cell& c,float unknownCost,bool familiarMap) {
  // Two-metre familiar tiles mix walls and passages. Their existing familiar
  // channel selects a soft prior; fresh live observations have already won in
  // memory. Second/final candidate: familiar free 2, unknown 4, danger 4.25;
  // the weaker hazard premium avoids overpricing mixed passage tiles.
  if(c.familiar!=0) {
    if(!familiarMap) return unknownCost;
    if(familiarDanger(c)) return unknownCost*1.0625f;
    if(c.channels[1]!=0) return std::max(1.f,unknownCost*.5f);
    return unknownCost;
    }
  // Optimism: an unobserved cell costs four times clear observed ground,
  // rather than becoming an artificial wall at the edge of perception.
  const float knownFree=float(std::min(c.channels[0],c.channels[1]))/255.f;
  return 1.f+(unknownCost-1.f)*(1.f-knownFree);
  }
bool insideNear(Point p) {
  return std::abs(p.rightCm)<NearExtent && std::abs(p.forwardCm)<NearExtent;
  }
Point center(int index) {
  const bool near=index<NearCount;
  if(!near) index-=NearCount;
  const int size=near ? NearSize : FarSize;
  const float cell=near ? NearCell : FarCell;
  return {(float(index%size)+.5f-float(size)*.5f)*cell,
          (float(index/size)+.5f-float(size)*.5f)*cell};
  }
const Cell& cell(const Input& in,int i) {
  return i<NearCount ? in.near[size_t(i)] : in.far[size_t(i-NearCount)];
  }

class Grid final {
  public:
    explicit Grid(const Input& input):Grid(input,DiagnosticsOptions{}) {}
    Grid(const Input& input,const DiagnosticsOptions& parameters)
      :in(input),options(parameters),blocked(Count),active(Count),costs(Count) {
      const char* enabled=std::getenv("OPENGOTHIC_NAV_FAMILIAR_MAP");
      familiarMap=enabled && std::string_view(enabled)=="1";
      waterLayers={std::any_of(in.near.begin(),in.near.end(),liveWater),
                   std::any_of(in.far.begin(),in.far.end(),liveWater)};
      // Only already occupied near cells touching the current body footprint
      // lose their inflation. Their raw square remains blocked. The exemption
      // cannot propagate to the rest of a wall or to far coarse cells.
      for(int i=0;i<NearCount;++i) {
        if(!dangerous(cell(in,i))) continue;
        // Water must never use the occupied-start exemption to step deeper
        // through adjacent wet cells. Stop before entering its safety margin.
        if(liveWater(cell(in,i))) continue;
        const Point p=center(i);
        const float dx=std::max(0.f,std::abs(p.rightCm)-NearCell*.5f);
        const float dz=std::max(0.f,std::abs(p.forwardCm)-NearCell*.5f);
        if(dx*dx+dz*dz<=options.bodyRadiusCm*options.bodyRadiusCm) {
          bodyOverlap[size_t(i)]=1;
          ++overlapCount;
          }
        }
      for(int i=0;i<Count;++i) {
        const Point p=center(i);
        active[size_t(i)]=i<NearCount || (!options.nearOnly && !insideNear(p));
        blocked[size_t(i)]=!clear(p);
        costs[size_t(i)]=cost(cell(in,i),options.unknownCost,familiarMap);
        }
      }
    void statistics(Diagnostics& result) const {
      std::array<uint32_t,2> activeCount={},blockedCount={},dangerCount={},freeCount={};
      for(int i=0;i<Count;++i) {
        const size_t layer=i<NearCount ? 0 : 1;
        dangerCount[layer]+=dangerous(cell(in,i));
        freeCount[layer]+=knownFree(cell(in,i),familiarMap);
        if(active[size_t(i)]) {
          ++activeCount[layer];
          blockedCount[layer]+=blocked[size_t(i)]!=0;
          }
        }
      for(size_t layer=0;layer<2;++layer) {
        const float total=layer==0 ? float(NearCount) : float(Count-NearCount);
        result.dangerFraction[layer]=float(dangerCount[layer])/total;
        result.knownFreeFraction[layer]=float(freeCount[layer])/total;
        result.blockedFraction[layer]=activeCount[layer]>0 ? float(blockedCount[layer])/float(activeCount[layer]) : 0.f;
        }
      }
    // Exact circle versus occupied cell square. Near/far retain their native
    // resolutions; far cells inside the near footprint cannot bypass a near
    // obstacle. The production 15-cm margin also applies across the seam.
    bool clear(Point p) const { return clear(p,options.bodyRadiusCm); }
    bool clear(Point p,float radius,bool leavingBodyOverlap=false) const {
      const float extent=options.nearOnly ? NearExtent : 10000.f;
      if(std::abs(p.rightCm)>extent-radius || std::abs(p.forwardCm)>extent-radius) return false;
      const auto layer=[&](bool near) {
        if(!near && options.nearOnly) return true;
        const int size=near ? NearSize : FarSize;
        const float step=near ? NearCell : FarCell;
        const bool waterMargin=radius>0 && waterLayers[near ? 0 : 1];
        const float searchRadius=waterMargin ? std::max(radius,WaterMarginCm) : radius;
        const int x0=std::max(0,int(std::floor((p.rightCm-searchRadius)/step+float(size)*.5f)));
        const int x1=std::min(size-1,int(std::floor((p.rightCm+searchRadius)/step+float(size)*.5f)));
        const int z0=std::max(0,int(std::floor((p.forwardCm-searchRadius)/step+float(size)*.5f)));
        const int z1=std::min(size-1,int(std::floor((p.forwardCm+searchRadius)/step+float(size)*.5f)));
        for(int z=z0;z<=z1;++z)for(int x=x0;x<=x1;++x) {
          const int index=z*size+x+(near ? 0 : NearCount);
          const Point q=center(index);
          if(!near && insideNear(q)) continue;
          const auto& evidence=cell(in,index);
          if(!dangerous(evidence) && (!options.knownFreeOnly || knownFree(evidence,familiarMap))) continue;
          if(near && bodyOverlap[size_t(index)]) {
            if(leavingBodyOverlap) continue;
            // Half-open raw occupancy, matching the memory cell addressing.
            // A neighbour merely touching the origin is not its raw start cell.
            if(p.rightCm>=q.rightCm-step*.5f && p.rightCm<q.rightCm+step*.5f &&
               p.forwardCm>=q.forwardCm-step*.5f && p.forwardCm<q.forwardCm+step*.5f) return false;
            continue;
            }
          const float dx=std::max(0.f,std::abs(p.rightCm-q.rightCm)-step*.5f);
          const float dz=std::max(0.f,std::abs(p.forwardCm-q.forwardCm)-step*.5f);
          const float margin=waterMargin && liveWater(evidence) ? std::max(radius,WaterMarginCm) : radius;
          if(dx*dx+dz*dz<=margin*margin) return false;
          }
        return true;
        };
      return layer(true) && layer(false);
      }
    bool segment(Point a,Point b,bool escape=false) const {
      const int steps=std::max(1,int(std::ceil(distance(a,b)/(NearCell*.5f))));
      bool leftInitialInflation=!escape || clear(a);
      for(int k=0;k<=steps;++k) {
        const float t=float(k)/float(steps);
        const Point p={a.rightCm+(b.rightCm-a.rightCm)*t,a.forwardCm+(b.forwardCm-a.forwardCm)*t};
        if(clear(p)) {leftInitialInflation=true;continue;}
        // A raw occupied start may leave ONLY the finite occupied footprint
        // found at this observation's body origin. Other raw hazards retain
        // their full margin; after reaching clear ground, re-entry is forbidden.
        if(leftInitialInflation || !clear(p,options.bodyRadiusCm,true)) return false;
        }
      return true;
      }
    struct Start final {
      int index = -1;
      PlanStatus status;
      };
    Start start() const {
      Start result;
      result.status.escapeRequired=!clear({},0);
      result.status.overlapDangerCells=overlapCount;
      float nearest=Infinity;
      const float limit=result.status.escapeRequired ? 100.f : 125.f;
      for(int i=0;i<NearCount;++i) {
        if(!available(i)) continue;
        const Point p=center(i);
        const float d=length(p);
        if(d<nearest && d<=limit && segment({},p,true)) {
          nearest=d;result.index=i;result.status.escapeTarget=p;
          }
        }
      result.status.startFound=result.index>=0;
      return result;
      }
    bool available(int i) const { return i>=0 && active[size_t(i)] && !blocked[size_t(i)]; }
    float traversal(int i) const { return costs[size_t(i)]; }
    template<class F> void neighbors(int i,F visit) const {
      const bool near=i<NearCount;
      const int local=near ? i : i-NearCount,size=near ? NearSize : FarSize;
      const int x=local%size,z=local/size;
      const Point p=center(i);
      const auto edge=[&](int j) {
        if(available(j) && segment(p,center(j))) visit(j);
        };
      for(int dz=-1;dz<=1;++dz)for(int dx=-1;dx<=1;++dx) {
        if((dx==0 && dz==0) || x+dx<0 || z+dz<0 || x+dx>=size || z+dz>=size) continue;
        edge((z+dz)*size+x+dx+(near ? 0 : NearCount));
        }
      if(near && (x==0 || z==0 || x==NearSize-1 || z==NearSize-1)) {
        const int fx=int(std::floor(p.rightCm/FarCell+float(FarSize)*.5f));
        const int fz=int(std::floor(p.forwardCm/FarCell+float(FarSize)*.5f));
        for(int dz=-1;dz<=1;++dz)for(int dx=-1;dx<=1;++dx) {
          if(fx+dx<0 || fz+dz<0 || fx+dx>=FarSize || fz+dz>=FarSize) continue;
          edge(NearCount+(fz+dz)*FarSize+fx+dx);
          }
        }
      if(!near && std::abs(p.rightCm)<=1100 && std::abs(p.forwardCm)<=1100) {
        const int nx=std::clamp(int(std::floor(p.rightCm/NearCell+float(NearSize)*.5f)),0,NearSize-1);
        const int nz=std::clamp(int(std::floor(p.forwardCm/NearCell+float(NearSize)*.5f)),0,NearSize-1);
        for(int dz=-2;dz<=2;++dz)for(int dx=-2;dx<=2;++dx) {
          if(nx+dx<0 || nz+dz<0 || nx+dx>=NearSize || nz+dz>=NearSize) continue;
          edge((nz+dz)*NearSize+nx+dx);
          }
        }
      }
  private:
    const Input& in;
    const DiagnosticsOptions options;
    bool familiarMap = false;
    std::array<bool,2> waterLayers = {};
    std::array<uint8_t,NearCount> bodyOverlap = {};
    uint32_t overlapCount = 0;
    std::vector<uint8_t> blocked,active;
    std::vector<float> costs;
  };

// The body pushed this way without moving (a point RefusedCm along the step).
bool refusedStep(const std::vector<Point>& pushed,Point step) {
  const float extent=length(step);
  if(extent<=0) return false;
  const Point probe={step.rightCm*RefusedCm/extent,step.forwardCm*RefusedCm/extent};
  return std::any_of(pushed.begin(),pushed.end(),[&](Point p) {return distance(p,probe)<10.f;});
  }

int nearIndex(Point p) {
  const int x=int(std::floor(p.rightCm/NearCell+float(NearSize)*.5f));
  const int z=int(std::floor(p.forwardCm/NearCell+float(NearSize)*.5f));
  return x<0 || z<0 || x>=NearSize || z>=NearSize ? -1 : z*NearSize+x;
  }
float squareDistance(Point p,int index) {
  const Point q=center(index);
  const float dx=std::max(0.f,std::abs(p.rightCm-q.rightCm)-NearCell*.5f);
  const float dz=std::max(0.f,std::abs(p.forwardCm-q.forwardCm)-NearCell*.5f);
  return std::hypot(dx,dz);
  }

// The body testifies. No start cell means the memory holds a hazard on every
// short way out of where the feet stand; the body stands there all the same.
// The cells its footprint covers now, and covered along its own recent track,
// are passable even where memory holds a static hazard (a person who may since
// stand on the track is not). Other cells are passable when they hold no raw
// hazard: the 15-cm margin is planning comfort, the capsule is 8.2 cm.
// The exit ends at a cell whose whole square is clear with the full margin, so
// the next decision finds a start there: the one with the least exit cost plus
// straight distance to the teacher's target (as A* would rank it), so that the
// exit is chosen alike from neighbouring places. Live water keeps its stop.
class BodyExit final {
  public:
    BodyExit(const Input& input,const Grid& planner,const std::vector<Point>& ownTrack,const std::vector<Point>& pushed)
      :in(input),grid(planner) {
      testify({},{},Footprint);
      for(size_t i=1;i<ownTrack.size();++i) testify(ownTrack[i-1],ownTrack[i],Track);
      // Where the body pushed without moving, it testifies the opposite.
      for(const auto& p:pushed) {
        const int i=nearIndex(p);
        if(i>=0) refused[size_t(i)]=1;
        }
      }
    bool waterNear() const {
      for(int i=0;i<NearCount;++i)
        if(liveWater(cell(in,i)) && squareDistance({},i)<=WaterMarginCm) return true;
      return false;
      }
    bool passable(int i) const {
      if(i<0 || refused[size_t(i)]) return false;
      const auto& c=cell(in,i);
      if(liveWater(c)) return false;
      if(!dangerous(c) || testified[size_t(i)]==Footprint) return true;
      return testified[size_t(i)]==Track && c.channels[6]==0;
      }
    bool passable(Point a,Point b) const {
      const int steps=std::max(1,int(std::ceil(distance(a,b)/(NearCell*.5f))));
      for(int k=0;k<=steps;++k) {
        const float t=float(k)/float(steps);
        if(!passable(nearIndex({a.rightCm+(b.rightCm-a.rightCm)*t,a.forwardCm+(b.forwardCm-a.forwardCm)*t}))) return false;
        }
      return true;
      }
    // Origin first, then near cell centres up to the exit cell; empty if none within
    // ExitCm. Exit cells within FailedCm of `failed` are not chosen.
    std::vector<Point> path(Point target,const std::vector<Point>& failed) const {
      std::vector<float> best(NearCount,Infinity),along(NearCount,Infinity);
      std::vector<int> parent(NearCount,-1);
      std::vector<uint8_t> closed(NearCount);
      struct Entry final {float score=0;int index=0;};
      struct Later final {
        bool operator()(const Entry& a,const Entry& b) const {
          return a.score!=b.score ? a.score>b.score : a.index>b.index;
          }
        };
      std::priority_queue<Entry,std::vector<Entry>,Later> queue;
      for(int z=NearSize/2-1;z<=NearSize/2;++z)for(int x=NearSize/2-1;x<=NearSize/2;++x) {
        const int i=z*NearSize+x;
        if(!passable(i)) continue;
        best[size_t(i)]=along[size_t(i)]=length(center(i));
        queue.push({best[size_t(i)],i});
        }
      const auto goal=[&](int i) {
        return grid.available(i) && grid.clear(center(i),FootprintCm+NearCell*.5f);
        };
      int chosen=-1;
      float chosenScore=Infinity;
      while(!queue.empty()) {
        const int i=queue.top().index;queue.pop();
        if(closed[size_t(i)]) continue;
        closed[size_t(i)]=1;
        if(goal(i)) {
          // An exit cell ends its branch; beyond it the planner takes over.
          const bool refused=std::any_of(failed.begin(),failed.end(),[&](Point f) {return distance(f,center(i))<FailedCm;});
          const float score=best[size_t(i)]+distance(center(i),target);
          if(!refused && score<chosenScore) {chosenScore=score;chosen=i;}
          continue;
          }
        const int x=i%NearSize,z=i/NearSize;
        for(int dz=-1;dz<=1;++dz)for(int dx=-1;dx<=1;++dx) {
          if((dx==0 && dz==0) || x+dx<0 || z+dz<0 || x+dx>=NearSize || z+dz>=NearSize) continue;
          const int j=(z+dz)*NearSize+x+dx;
          if(closed[size_t(j)] || !passable(j)) continue;
          // No corner cutting: a diagonal step needs both orthogonal neighbours.
          if(dx!=0 && dz!=0 && (!passable(z*NearSize+x+dx) || !passable((z+dz)*NearSize+x))) continue;
          const float step=distance(center(i),center(j));
          if(along[size_t(i)]+step>ExitCm) continue;
          // A testified hazard is passable, but raw-free ground is preferred.
          const float score=best[size_t(i)]+step+(dangerous(cell(in,j)) ? NearCell : 0.f);
          if(score>=best[size_t(j)]) continue;
          best[size_t(j)]=score;along[size_t(j)]=along[size_t(i)]+step;parent[size_t(j)]=i;
          queue.push({score,j});
          }
        }
      if(chosen<0) return {};
      std::vector<Point> result;
      for(int j=chosen;j>=0;j=parent[size_t(j)]) result.push_back(center(j));
      result.push_back({});
      std::reverse(result.begin(),result.end());
      return result;
      }
  private:
    enum : uint8_t { None, Track, Footprint };
    void testify(Point a,Point b,uint8_t kind) {
      const int steps=std::max(1,int(std::ceil(distance(a,b)/5.f)));
      for(int k=0;k<=steps;++k) {
        const float t=float(k)/float(steps);
        const Point p={a.rightCm+(b.rightCm-a.rightCm)*t,a.forwardCm+(b.forwardCm-a.forwardCm)*t};
        const int x0=std::max(0,int(std::floor((p.rightCm-FootprintCm)/NearCell+float(NearSize)*.5f)));
        const int x1=std::min(NearSize-1,int(std::floor((p.rightCm+FootprintCm)/NearCell+float(NearSize)*.5f)));
        const int z0=std::max(0,int(std::floor((p.forwardCm-FootprintCm)/NearCell+float(NearSize)*.5f)));
        const int z1=std::min(NearSize-1,int(std::floor((p.forwardCm+FootprintCm)/NearCell+float(NearSize)*.5f)));
        for(int z=z0;z<=z1;++z)for(int x=x0;x<=x1;++x) {
          const int i=z*NearSize+x;
          if(squareDistance(p,i)<=FootprintCm) testified[size_t(i)]=std::max(testified[size_t(i)],kind);
          }
        }
      }
    const Input& in;
    const Grid& grid;
    std::array<uint8_t,NearCount> testified = {}, refused = {};
  };

// One step along the body exit without turning (NpcKeys executes one axis;
// a turn would move every remembered cell of the body frame). Returns false
// when there is no exit; then the teacher keeps its previous behaviour.
bool bodyExitKeys(const Input& in,const Grid& grid,const std::vector<Point>& track,const std::vector<Point>& pushed,
                  Point target,const std::vector<Point>& failed,NpcKeys::KeyState& out,float& exitCm,Point& exitCell) {
  exitCm=0;
  const BodyExit exit(in,grid,track,pushed);
  if(exit.waterNear()) return false;
  const auto path=exit.path(target,failed);
  if(path.size()<2) return false;
  exitCell=path.back();
  for(size_t i=1;i<path.size();++i) exitCm+=distance(path[i-1],path[i]);
  Point aim=path.back();
  for(size_t i=1;i<path.size();++i)
    if(length(path[i])>=35.f) {aim=path[i];break;}
  const bool sidewaysFirst=std::abs(aim.rightCm)>std::abs(aim.forwardCm);
  for(unsigned attempt=0;attempt<2;++attempt) {
    const bool sideways=attempt==0 ? sidewaysFirst : !sidewaysFirst;
    const float component=sideways ? aim.rightCm : aim.forwardCm;
    if(component==0) continue;
    const float extent=std::clamp(std::abs(component),25.f,60.f);
    const Point step=sideways ? Point{sign(component)*extent,0} : Point{0,sign(component)*extent};
    if(!exit.passable({},step) || refusedStep(pushed,step)) continue;
    if(sideways) out.strafe=sign(component);
    else out.forward=sign(component);
    return true;
    }
  // Neither axis step stays on the exit: face its first stretch instead.
  const float error=std::atan2(aim.rightCm,aim.forwardCm)*180.f/Pi;
  out.turn=-sign(error);
  return true;
  }

void plan(const Input& in,Point target,std::vector<Point>& route,float& routeLength,bool& complete,
          const DiagnosticsOptions& options=DiagnosticsOptions{},Diagnostics* diagnostics=nullptr,
          PlanStatus* planStatus=nullptr) {
  Grid grid(in,options);
  // Production retains its exact 9700-cm proxy. Near-only diagnostic deliberately
  // stops at a 700-cm proxy, with no claim about the target outside that map.
  const float proxyExtent=options.nearOnly ? 700.f : 9700.f;
  const float scale=std::max({1.f,std::abs(target.rightCm)/proxyExtent,std::abs(target.forwardCm)/proxyExtent});
  const Point localTarget={target.rightCm/scale,target.forwardCm/scale};
  if(diagnostics!=nullptr) {
    diagnostics->target=target;diagnostics->localTarget=localTarget;diagnostics->targetScale=scale;
    diagnostics->targetClamped=scale>1;
    const float extent=options.nearOnly ? NearExtent : 10000.f;
    diagnostics->targetOutsideMap=std::abs(target.rightCm)>=extent || std::abs(target.forwardCm)>=extent;
    diagnostics->startClear=grid.clear({});diagnostics->targetClear=grid.clear(target);
    diagnostics->snappedTargetDistanceCm=Infinity;
    diagnostics->endpointDistanceToActualTargetCm=length(target);
    grid.statistics(*diagnostics);
    }
  const auto startInfo=grid.start();
  const int start=startInfo.index;
  if(planStatus!=nullptr) *planStatus=startInfo.status;
  int goal=-1;
  float goalDistance=Infinity;
  for(int i=0;i<Count;++i) {
    if(!grid.available(i)) continue;
    const Point p=center(i);
    const float d=distance(p,localTarget);
    if(d<goalDistance) {goalDistance=d;goal=i;}
    }
  route.clear();route.push_back({});routeLength=0;complete=false;
  if(diagnostics!=nullptr) {
    diagnostics->startFound=startInfo.status.startFound;
    diagnostics->startEscapeRequired=startInfo.status.escapeRequired;
    diagnostics->startEscapeTarget=startInfo.status.escapeTarget;
    diagnostics->overlapDangerCells=startInfo.status.overlapDangerCells;
    if(goal>=0) diagnostics->snappedTarget=center(goal);
    diagnostics->snappedTargetDistanceCm=goalDistance;
    }
  if(start<0 || goal<0) return;
  struct Entry final {float score=0;int index=0;};
  struct Later final {
    bool operator()(const Entry& a,const Entry& b) const {
      return a.score!=b.score ? a.score>b.score : a.index>b.index;
      }
    };
  std::vector<float> best(Count,Infinity);
  std::vector<int> parent(Count,-1);
  std::vector<uint8_t> closed(Count);
  std::priority_queue<Entry,std::vector<Entry>,Later> queue;
  best[size_t(start)]=0;
  queue.push({distance(center(start),center(goal)),start});
  int closest=start;
  float closestDistance=distance(center(start),center(goal));
  while(!queue.empty()) {
    const int i=queue.top().index;queue.pop();
    if(closed[size_t(i)]) continue;
    closed[size_t(i)]=1;
    if(diagnostics!=nullptr) ++diagnostics->reachableNodes;
    const float remaining=distance(center(i),center(goal));
    if(remaining<closestDistance) {closestDistance=remaining;closest=i;}
    if(i==goal) {
      closest=i;complete=true;
      if(diagnostics==nullptr) break;
      }
    grid.neighbors(i,[&](int j) {
      if(closed[size_t(j)]) return;
      const float score=best[size_t(i)]+distance(center(i),center(j))*(grid.traversal(i)+grid.traversal(j))*.5f;
      if(score>=best[size_t(j)]) return;
      best[size_t(j)]=score;parent[size_t(j)]=i;
      queue.push({score+distance(center(j),center(goal)),j});
      });
    }
  std::vector<Point> reverse;
  for(int i=closest;i>=0;i=parent[size_t(i)]) reverse.push_back(center(i));
  route.insert(route.end(),reverse.rbegin(),reverse.rend());
  const bool actualTargetReached=complete && scale==1 && grid.segment(route.back(),target);
  if(actualTargetReached) route.push_back(target);
  for(size_t i=1;i<route.size();++i)routeLength+=distance(route[i-1],route[i]);
  if(diagnostics!=nullptr) {
    diagnostics->actualTargetReached=actualTargetReached;
    diagnostics->endpointDistanceToActualTargetCm=distance(route.back(),target);
    diagnostics->planCost=best[size_t(closest)];
    }
  }

void quantize(const NavigationMemory::Layer& from,size_t size,float step,NavMemoryTeacher::Cell* out) {
  if(from.size!=size || from.cells.size()!=size*size || from.cellCm!=step)
    throw std::runtime_error("memory teacher: incorrect learner memory shape");
  for(size_t i=0;i<from.cells.size();++i) {
    const auto& c=from.cells[i];
    const float channels[]={c.confidence,c.free,c.wall,c.ledge,c.drop,c.water,c.movable};
    for(size_t k=0;k<7;++k) {
      if(!std::isfinite(channels[k]) || channels[k]<0 || channels[k]>1)
        throw std::runtime_error("memory teacher: invalid learner memory channel");
      out[i].channels[k]=uint8_t(std::lround(channels[k]*255.f));
      }
    if(!std::isfinite(c.familiar) || c.familiar<0 || c.familiar>1)
      throw std::runtime_error("memory teacher: invalid learner familiarity");
    out[i].familiar=uint8_t(std::lround(c.familiar*255.f));
    if(!std::isfinite(c.groundHeightCm) && !std::isnan(c.groundHeightCm))
      throw std::runtime_error("memory teacher: infinite learner memory height");
    out[i].heightValid=std::isfinite(c.groundHeightCm);
    out[i].groundHeightCm=out[i].heightValid ? c.groundHeightCm : 0.f;
    }
  }
}

NavMemoryTeacher::Input NavMemoryTeacher::Input::fromLearner(const NavDemoWriter::Body& body,
    const NavDemoWriter::Roads& roads,const NpcKeys::KeyState& previous,const NavigationMemory::View& memory) {
  Input result;
  result.body=body;result.roads=roads;result.previous=previous;
  quantize(memory.near,64,25,result.near.data());
  quantize(memory.far,100,200,result.far.data());
  return result;
  }

NavMemoryTeacher::Diagnostics NavMemoryTeacher::probe(const Input& input,const DiagnosticsOptions& options) {
  if(!std::isfinite(options.bodyRadiusCm) || options.bodyRadiusCm<0 || options.bodyRadiusCm>200 ||
     !std::isfinite(options.unknownCost) || options.unknownCost<1)
    throw std::runtime_error("memory teacher diagnostics: invalid analysis options");
  const auto& direction=input.roads.beyondKnown ? input.roads.goalDirection : input.roads.carrotDirection;
  const float targetDistance=input.roads.beyondKnown ? input.roads.goalDistanceCm : input.roads.carrotDistanceCm;
  const Point target={direction[0]*targetDistance,direction[2]*targetDistance};
  if(!std::isfinite(target.rightCm) || !std::isfinite(target.forwardCm) || !std::isfinite(targetDistance) || targetDistance<0)
    throw std::runtime_error("memory teacher diagnostics: invalid learner target");
  Diagnostics result;
  ::plan(input,target,result.path,result.planLengthCm,result.planComplete,options,&result);
  return result;
  }

const char* NavMemoryTeacher::name(Branch branch) {
  switch(branch) {
    case Branch::Airborne: return "airborne";
    case Branch::AtGoal: return "at_goal";
    case Branch::StartNotFound: return "start_not_found";
    case Branch::Escape: return "escape";
    case Branch::EscapeBlocked: return "escape_blocked";
    case Branch::BodyExit: return "body_exit";
    case Branch::Standoff: return "standoff";
    case Branch::Recovery: return "recovery";
    case Branch::RecoveryBlocked: return "recovery_blocked";
    case Branch::NoAim: return "no_aim";
    case Branch::Behind: return "behind";
    case Branch::BehindBlocked: return "behind_blocked";
    case Branch::Strafe: return "strafe";
    case Branch::Forward: return "forward";
    case Branch::Turn: return "turn";
    case Branch::ForwardBlocked: return "forward_blocked";
    case Branch::SideBlocked: return "side_blocked";
    case Branch::Count: break;
    }
  return "invalid";
  }

void NavMemoryTeacher::reset() {
  route.clear();routeLength=0;stuckTicks=0;recoveryTicks=0;recoveryAttempts=0;
  recoverySide=1;complete=false;observedGoal=false;previousGoal={};movingTicks=0;runningTargetTicks=0;
  latestPlanStatus=PlanStatus{};branch=Branch::NoAim;track.clear();refused.clear();pushTicks=0;
  failedExits.clear();exitPending=false;exitCell={};
  }

void NavMemoryTeacher::updateTrack(const Input& in) {
  // Off the ground the feet testify nothing about the cells below.
  if(in.body.flags[0] || in.body.flags[1] || in.body.flags[3] || in.body.flags[4]) {
    track.clear();refused.clear();failedExits.clear();exitPending=false;
    return;
    }
  // The same motion model as the observed goal below: the previous turn key
  // and the observed velocity carry the old frame into the current one.
  const float angle=-in.previous.turn*TurnDegrees*Pi/180.f;
  const float c=std::cos(angle),s=std::sin(angle);
  const auto carry=[&](Point p) {
    return Point{p.rightCm*c-p.forwardCm*s-in.body.velocityCmS[0]*.1f,p.rightCm*s+p.forwardCm*c-in.body.velocityCmS[2]*.1f};
    };
  for(auto& p:track) p=carry(p);
  exitCell=carry(exitCell);
  for(auto* list:{&refused,&failedExits}) {
    for(auto& r:*list) {r.point=carry(r.point);--r.ttl;}
    list->erase(std::remove_if(list->begin(),list->end(),[](const Refusal& r) {
      return r.ttl==0 || length(r.point)>ExitCm;
      }),list->end());
    }
  // Standing still does not push older footprints out of the track.
  if(!track.empty() && length(track.back())<2.f) track.back()={};
  else track.push_back({});
  float along=0;
  size_t oldest=track.size()-1;
  while(oldest>0 && track.size()-oldest<TrackPoints) {
    along+=distance(track[oldest],track[oldest-1]);
    if(along>TrackCm) break;
    --oldest;
    }
  track.erase(track.begin(),track.begin()+std::ptrdiff_t(oldest));
  }

NpcKeys::KeyState NavMemoryTeacher::decide(const Input& in) {
  NpcKeys::KeyState out;
  out.walk=true;
  const auto& direction=in.roads.beyondKnown ? in.roads.goalDirection : in.roads.carrotDirection;
  const float targetDistance=in.roads.beyondKnown ? in.roads.goalDistanceCm : in.roads.carrotDistanceCm;
  const Point target={direction[0]*targetDistance,direction[2]*targetDistance};
  if(!std::isfinite(target.rightCm) || !std::isfinite(target.forwardCm) || !std::isfinite(targetDistance) || targetDistance<0)
    throw std::runtime_error("memory teacher: non-finite learner goal");
  // Rebuild from the full quantized observation on EVERY 100-ms decision.
  ::plan(in,target,route,routeLength,complete,DiagnosticsOptions{},nullptr,&latestPlanStatus);
  updateTrack(in);
  const float speed=std::hypot(in.body.velocityCmS[0],in.body.velocityCmS[2]);
  if(bodyExitEnabled) {
    // The body testifies where an escape step pushed without moving it (its
    // previous decision: no turn, so the step is in the current frame too).
    const bool pushed=(branch==Branch::Escape || branch==Branch::BodyExit) &&
                      (in.previous.forward!=0 || in.previous.strafe!=0) && speed<StuckSpeed;
    pushTicks=pushed ? pushTicks+1 : 0;
    if(pushTicks>=PushTicks) {
      refused.push_back({in.previous.strafe!=0 ? Point{sign(in.previous.strafe)*RefusedCm,0}
                                               : Point{0,sign(in.previous.forward)*RefusedCm},RefusedTtl});
      pushTicks=0;
      }
    }

  const Point goal={in.roads.goalDirection[0]*in.roads.goalDistanceCm,
                    in.roads.goalDirection[2]*in.roads.goalDistanceCm};
  if(observedGoal) {
    const float angle=-in.previous.turn*9.f*Pi/180.f;
    const float c=std::cos(angle),s=std::sin(angle);
    const Point predicted={previousGoal.rightCm*c-previousGoal.forwardCm*s-in.body.velocityCmS[0]*.1f,
                           previousGoal.rightCm*s+previousGoal.forwardCm*c-in.body.velocityCmS[2]*.1f};
    const float targetStep=distance(goal,predicted);
    if(targetStep>8 && targetStep<120) movingTicks=5;
    else if(movingTicks>0) --movingTicks;
    // This is the target's observed displacement after subtracting our own
    // movement, not its hidden native gait or world velocity. Retain a running
    // observation briefly through a turn or the target's next acceleration.
    if(targetStep>22 && targetStep<120) runningTargetTicks=5;
    else if(runningTargetTicks>0) --runningTargetTicks;
    }
  observedGoal=true;previousGoal=goal;
  // Motion is inferred exclusively from learner observations. The previous
  // recorded action carries the gait hysteresis; no scenario mode is read.
  if(movingTicks>0) {
    out.walk=in.previous.walk;
    if(in.roads.goalDistanceCm>400 || (runningTargetTicks>0 && in.roads.goalDistanceCm>300)) out.walk=false;
    else if(in.roads.goalDistanceCm<300) out.walk=true;
    }

  if(in.body.flags[0] || in.body.flags[1] || in.body.flags[3]) {
    branch=Branch::Airborne;
    return out;
    }
  if(in.roads.goalDistanceCm<75) {
    stuckTicks=0;recoveryTicks=0;branch=Branch::AtGoal;
    return out;
    }

  Grid grid(in);
  std::vector<Point> pushed,failed;
  for(const auto& r:refused) pushed.push_back(r.point);
  if(bodyExitEnabled && exitPending && length(exitCell)<ReachedCm && !latestPlanStatus.startFound) {
    // The body reached its exit cell and still finds no start there.
    failedExits.push_back({exitCell,FailedTtl});
    exitPending=false;
    }
  if(latestPlanStatus.startFound) exitPending=false;
  for(const auto& r:failedExits) failed.push_back(r.point);
  const auto exitBody=[&](NpcKeys::KeyState& keys) {
    // No start cell, or no clear escape step: leave along what the body testifies.
    latestPlanStatus.bodyExit=true;
    auto exit=out;
    exit.walk=true;
    if(!bodyExitKeys(in,grid,track,pushed,target,failed,exit,latestPlanStatus.bodyExitCm,exitCell)) return false;
    exitPending=true;
    stuckTicks=0;recoveryTicks=0;branch=Branch::BodyExit;keys=exit;
    return true;
    };
  if(bodyExitEnabled && !latestPlanStatus.startFound && exitBody(out)) return out;
  if(latestPlanStatus.escapeRequired) {
    // Exit the raw occupied start before distant steering or recovery. Digital
    // strafe overrides forward in NpcKeys, so select one physically clear axis
    // and hold the body yaw fixed throughout this initial step.
    stuckTicks=0;recoveryTicks=0;out.walk=true;
    if(!latestPlanStatus.startFound) {
      branch=Branch::StartNotFound;
      return out;
      }
    const Point exit=latestPlanStatus.escapeTarget;
    const bool sidewaysFirst=std::abs(exit.rightCm)>std::abs(exit.forwardCm);
    for(unsigned attempt=0;attempt<2;++attempt) {
      const bool sideways=attempt==0 ? sidewaysFirst : !sidewaysFirst;
      const float component=sideways ? exit.rightCm : exit.forwardCm;
      if(component==0) continue;
      for(float extent=30;extent<=100;extent+=10) {
        const Point step=sideways ? Point{sign(component)*extent,0} : Point{0,sign(component)*extent};
        if(!grid.clear(step) || !grid.segment({},step,true)) continue;
        if(bodyExitEnabled && refusedStep(pushed,step)) break;
        if(sideways) out.strafe=sign(component);
        else out.forward=sign(component);
        branch=Branch::Escape;
        return out;
        }
      }
    if(bodyExitEnabled && exitBody(out)) return out;
    branch=Branch::EscapeBlocked;
    return out;
    }
  // Following exposes its stand-off point as the learner's carrot. Approaching
  // exposes the target itself, so nearby target motion must not prevent contact.
  if(movingTicks>0 && !in.roads.beyondKnown && targetDistance<50) {
    stuckTicks=0;recoveryTicks=0;branch=Branch::Standoff;
    // A target turning back toward the follower must not push it inside the
    // tracking band. Retreat using only currently clear remembered ground.
    const float angle=std::atan2(goal.rightCm,goal.forwardCm)*180.f/Pi;
    if(std::abs(angle)>10) out.turn=-sign(angle);
    if(in.roads.goalDistanceCm<220) {
      const bool sidewaysFirst=std::abs(goal.rightCm)>std::abs(goal.forwardCm);
      for(unsigned attempt=0;attempt<2;++attempt) {
        const bool sideways=attempt==0 ? sidewaysFirst : !sidewaysFirst;
        const float component=sideways ? goal.rightCm : goal.forwardCm;
        if(component==0) continue;
        const Point away=sideways ? Point{-sign(component)*70,0} : Point{0,-sign(component)*70};
        if(!grid.segment({},away,true)) continue;
        if(sideways) out.strafe=-sign(component);
        else out.forward=-sign(component);
        break;
        }
      }
    return out;
    }
  Point aim=target;
  if(route.size()>1) {
    // Look ahead through clear cells without cutting a memory obstacle corner.
    aim=route[1];
    for(size_t i=2;i<route.size();++i) {
      if(length(route[i])>175 || !grid.segment({},route[i],true)) break;
      aim=route[i];
      }
    }
  else if(!grid.segment({},aim,true)) aim={};
  const float error=std::atan2(aim.rightCm,aim.forwardCm)*180.f/Pi;
  if(length(aim)>1 && std::abs(error)>10) out.turn=-sign(error);

  const bool commanded=in.previous.forward!=0 || in.previous.strafe!=0;
  if(commanded && speed<18) ++stuckTicks;
  else stuckTicks=0;
  if(stuckTicks>=7 && recoveryTicks==0) {
    // Alternate only after a failed escape, never on successive decisions.
    ++recoveryAttempts;
    recoverySide=aim.rightCm==0 ? (recoveryAttempts%2==1 ? 1.f : -1.f) : sign(aim.rightCm);
    if(recoveryAttempts%2==0) recoverySide=-recoverySide;
    recoveryTicks=12;stuckTicks=0;
    }
  if(recoveryTicks>0) {
    // NpcKeys gives strafe priority over forward, so backward and sideways
    // are separate sustained phases, rather than a cosmetic diagonal label.
    const bool backing=recoveryTicks>6;
    const Point escape=backing ? Point{0,-80} : Point{recoverySide*80,0};
    branch=Branch::RecoveryBlocked;
    if(grid.segment({},escape,true)) {
      if(backing) out.forward=-1;
      else out.strafe=recoverySide;
      branch=Branch::Recovery;
      }
    out.walk=true;
    --recoveryTicks;
    return out;
    }
  if(length(aim)<1) {
    branch=latestPlanStatus.startFound ? Branch::NoAim : Branch::StartNotFound;
    return out;
    }
  if(aim.forwardCm<0 && std::abs(error)>110) {
    // A route behind the actor is demonstrated as physical backward motion,
    // followed by a side step while turning toward its first segment.
    branch=Branch::Behind;
    if(std::abs(aim.rightCm)>std::abs(aim.forwardCm)*.5f && grid.segment({}, {sign(aim.rightCm)*70,0},true))
      out.strafe=sign(aim.rightCm);
    else if(grid.segment({}, {0,-70},true)) out.forward=-1;
    else branch=Branch::BehindBlocked;
    return out;
    }
  if(std::abs(error)>55 && std::abs(error)<=110 && grid.segment({}, {sign(aim.rightCm)*70,0},true)) {
    out.strafe=sign(aim.rightCm);branch=Branch::Strafe;
    }
  else if(std::abs(error)<=35 && grid.segment({}, {std::sin(error*Pi/180.f)*70,std::cos(error*Pi/180.f)*70},true)) {
    out.forward=1;branch=Branch::Forward;
    }
  else branch=std::abs(error)<=35 ? Branch::ForwardBlocked : std::abs(error)<=55 ? Branch::Turn : Branch::SideBlocked;
  return out;
  }

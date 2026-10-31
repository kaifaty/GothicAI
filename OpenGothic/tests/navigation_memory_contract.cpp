#include "game/navigationmemory.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>

namespace {
using Tempest::Vec3;
using Memory = NavigationMemory;
using Ray = NavigationEye::Ray;
using Kind = NavigationEye::HitKind;
using Field = float Memory::Cell::*;

void require(bool condition, const char* message) {
  if(condition)
    return;
  std::fprintf(stderr,"FAIL: %s\n",message);
  std::exit(EXIT_FAILURE);
  }

void close(float actual, float expected, float tolerance, const char* message) {
  require(std::isfinite(actual) && std::abs(actual-expected)<=tolerance,message);
  }

Ray ray(const Vec3& point, Kind kind = Kind::World, float normalY = 0, uint64_t time = 0,
        const Vec3& origin = Vec3(0,150,0)) {
  Ray result;
  result.origin = origin;
  result.point = point;
  result.direction = Vec3::normalize(point-origin);
  result.distanceCm = (point-origin).length();
  result.maxRangeCm = result.distanceCm;
  result.sampledAtMs = result.nearSampledAtMs = time;
  result.kind = kind;
  result.normalY = normalY;
  result.hit = kind!=Kind::None;
  result.complete = true;
  return result;
  }

struct Found final { float right = 0, forward = 0, confidence = 0; };

Found strongest(const Memory::Layer& layer, Field field) {
  Found result;
  for(size_t z=0; z<layer.size; ++z)
    for(size_t x=0; x<layer.size; ++x) {
      const float value = layer.cells[z*layer.size+x].*field;
      if(value>result.confidence)
        result = {(float(x)+.5f-float(layer.size)/2.f)*layer.cellCm,
                  (float(z)+.5f-float(layer.size)/2.f)*layer.cellCm,value};
      }
  return result;
  }

float around(const Memory::Layer& layer, float right, float forward, Field field) {
  float result = 0;
  for(size_t z=0; z<layer.size; ++z)
    for(size_t x=0; x<layer.size; ++x) {
      const float dx = (float(x)+.5f-float(layer.size)/2.f)*layer.cellCm-right;
      const float dz = (float(z)+.5f-float(layer.size)/2.f)*layer.cellCm-forward;
      if(std::abs(dx)<=layer.cellCm && std::abs(dz)<=layer.cellCm)
        result = std::max(result,layer.cells[z*layer.size+x].*field);
      }
  return result;
  }

void turnHonestyAndFreeSpace() {
  Memory memory;
  Memory::View view;
  const std::vector<Ray> seen = {ray({500,150,0})};
  memory.update(seen,{{0,0,0},{1,0,0},0});
  memory.view(view);
  auto stone = strongest(view.near,&Memory::Cell::wall);
  close(stone.forward,500,25,"stone five metres in front");
  close(stone.right,0,25,"stone lateral position");
  for(float distance=50; distance<475; distance+=25)
    require(around(view.near,0,distance,&Memory::Cell::free)>.99f,"cells before a ray hit are free");
  require(around(view.near,150,300,&Memory::Cell::confidence)==0,"unseen obstacle and between-ray area remain unknown");
  memory.update(seen,{{0,0,0},{-1,0,0},1000});
  memory.view(view);
  stone = strongest(view.near,&Memory::Cell::wall);
  close(stone.forward,-500,25,"turning 180 degrees keeps stone five metres behind");
  close(stone.right,0,25,"turn preserves stone lateral position");
  std::printf("turn: stone behind %.1f cm, confidence %.6f; unseen=0, ray cells free\n",stone.forward,stone.confidence);
  }

void timeAndDistanceDecay() {
  Memory memory;
  Memory::View view;
  const std::vector<Ray> seen = {ray({5000,150,0}),ray({5000,150,600},Kind::MovableVob)};
  memory.update(seen,{{0,0,0},{1,0,0},0});
  memory.view(view);
  const float firstStatic = strongest(view.far,&Memory::Cell::wall).confidence;
  const float firstMovable = strongest(view.far,&Memory::Cell::movable).confidence;
  memory.update(seen,{{0,0,0},{1,0,0},5000});
  memory.view(view);
  const float timedStatic = strongest(view.far,&Memory::Cell::wall).confidence;
  const float timedMovable = strongest(view.far,&Memory::Cell::movable).confidence;
  require(timedStatic<firstStatic && timedMovable<firstMovable && timedMovable<timedStatic,
          "time decay is monotonic, movable objects fade faster");
  float previous = timedStatic;
  for(int metres=10; metres<=30; metres+=10) {
    memory.update(seen,{{float(metres)*100.f,0,0},{1,0,0},5000});
    memory.view(view);
    const float current = strongest(view.far,&Memory::Cell::wall).confidence;
    require(current<previous,"travelled path decay is monotonic");
    previous = current;
    }
  const auto stone = strongest(view.far,&Memory::Cell::wall);
  close(stone.forward,2000,200,"after thirty metres the fifty-metre object remains at the right relative position");
  close(stone.confidence,float(std::exp(-5./300.-.1)),1.e-5f,"time and path confidence formula");
  std::printf("decay: static %.6f -> %.6f, movable %.6f -> %.6f; after 30 m %.6f at %.1f cm\n",
              firstStatic,timedStatic,firstMovable,timedMovable,stone.confidence,stone.forward);
  }

void cachedRaysAndIncompleteRays() {
  Memory memory;
  Memory::View view;
  auto cached = ray({5000,150,0});
  memory.update({cached},{{0,0,0},{1,0,0},0});
  cached.nearSampledAtMs = 1000;
  memory.update({cached},{{3000,0,0},{-1,0,0},1000});
  memory.view(view);
  auto stone = strongest(view.far,&Memory::Cell::wall);
  close(stone.forward,-2000,200,"cached far ray keeps its world pose");
  close(stone.confidence,float(std::exp(-1./300.-.1)),1.e-5f,"cached far ray never refreshes its timestamp or path");
  auto freshNear = ray({3250,150,0},Kind::World,0,1100,{3000,150,0});
  memory.update({freshNear},{{3000,0,0},{-1,0,0},1100});
  memory.update({cached},{{3000,0,0},{-1,0,0},2000});
  memory.view(view);
  close(around(view.far,0,-2000,&Memory::Cell::wall),float(std::exp(-2./300.-.1)),1.e-5f,
        "an older far cache returning after a near hit is not ingested again");

  Memory partial;
  auto incomplete = ray({500,150,0},Kind::None);
  incomplete.complete = false;
  incomplete.maxRangeCm = 10000;
  partial.update({incomplete},{{0,0,0},{1,0,0},0});
  partial.view(view);
  require(around(view.near,0,400,&Memory::Cell::free)>.99f,"incomplete ray's observed near segment is free");
  require(around(view.near,0,650,&Memory::Cell::confidence)==0,"incomplete ray does not invent the uncast far segment");
  std::puts("cache: timestamp/pose retained, stale cache after new hit skipped; partial near segment retained");
  }

void groundAndLowEmptyAir() {
  Memory memory;
  Memory::View view;
  const std::vector<Ray> ground = {ray({300,80,100},Kind::World,1.f)};
  memory.update(ground,{{0,0,0},{1,0,0},0});
  memory.view(view);
  require(strongest(view.near,&Memory::Cell::ledge).confidence==0,"isolated ground above feet is not a ledge");
  require(around(view.near,100,300,&Memory::Cell::free)>.99f,"observed isolated ground is free");
  memory.update(ground,{{0,100,0},{1,0,0},1000});
  memory.view(view);
  require(strongest(view.near,&Memory::Cell::ledge).confidence==0,"moving feet does not invent a ledge");
  memory.update(ground,{{0,200,0},{1,0,0},2000});
  memory.view(view);
  require(strongest(view.near,&Memory::Cell::drop).confidence==0,"moving feet does not turn remembered ground into a drop");

  Memory hole;
  hole.update({ray({500,-200,0},Kind::None)},{{0,0,0},{1,0,0},0});
  hole.view(view);
  require(strongest(view.near,&Memory::Cell::drop).confidence==0,"low empty air without observed local ground proves no drop");
  hole.update({ray({450,0,0},Kind::World,1.f,1000)},{{0,0,0},{1,0,0},1000});
  hole.update({ray({500,-200,0},Kind::None,0,2000)},{{0,0,0},{1,0,0},2000});
  hole.view(view);
  require(strongest(view.near,&Memory::Cell::drop).confidence==0,"new low empty air does not turn stale ground into a drop");

  Memory vertical;
  vertical.update({ray({500,0,0},Kind::World,1.f),ray({500,150,100},Kind::World)},{{0,0,0},{1,0,0},0});
  vertical.update({ray({700,300,0},Kind::None,0,1000,{0,300,0}),
                   ray({700,300,140},Kind::None,0,1000,{0,300,0})},{{0,0,0},{1,0,0},1000});
  vertical.view(view);
  require(around(view.near,0,500,&Memory::Cell::free)>0,"high empty ray retains ground");
  require(around(view.near,100,500,&Memory::Cell::wall)>0,"high empty ray does not erase wall below it");
  vertical.update({ray({700,150,140},Kind::None,0,2000)},{{0,0,0},{1,0,0},2000});
  vertical.view(view);
  require(around(view.near,100,500,&Memory::Cell::wall)==0,"new empty ray through recorded wall height clears that wall");
  std::puts("ground: hazards independent of feet; low air alone proves no drop; high air preserves lower surfaces");
  }

void localStepsAndSlopes() {
  Memory::View view;
  for(float direction:{-1.f,1.f}) {
    Memory slope;
    std::vector<Ray> observations;
    for(int i=0; i<20; ++i) {
      const float x = 12.f+float(i)*25.f;
      observations.push_back(ray({x,direction*x*.4f,12.f},Kind::World,1.f,0,{x,500,12.f}));
      }
    for(int i=0; i<20; ++i) {
      const float x = 1012.f+float(i)*200.f;
      observations.push_back(ray({x,direction*x*.4f,12.f},Kind::World,1.f,0,{x,3000,12.f}));
      }
    slope.update(observations,{{0,0,0},{1,0,0},0});
    slope.view(view);
    for(const auto* layer:{&view.near,&view.far}) {
      require(strongest(*layer,&Memory::Cell::ledge).confidence==0,"continuous walkable slope is not a ledge");
      require(strongest(*layer,&Memory::Cell::drop).confidence==0,"continuous walkable slope is not a drop");
      require(strongest(*layer,&Memory::Cell::free).confidence>.99f,"continuous slope remains observed free ground");
      }
    }

  Memory edge;
  edge.update({ray({312,0,12},Kind::World,1.f),ray({337,80,12},Kind::World,1.f)},{{0,0,0},{1,0,0},0});
  edge.view(view);
  require(strongest(view.near,&Memory::Cell::ledge).confidence>.99f,"higher side of a local eighty-centimetre step is a ledge");
  require(strongest(view.near,&Memory::Cell::drop).confidence>.99f,"lower side of a local eighty-centimetre step is a drop");
  edge.update({},{{0,500,0},{1,0,0},1000});
  edge.view(view);
  require(strongest(view.near,&Memory::Cell::ledge).confidence>0 && strongest(view.near,&Memory::Cell::drop).confidence>0,
          "real discontinuity survives actor height changes");

  Memory step;
  step.update({ray({312,0,12},Kind::World,1.f),ray({337,50,12},Kind::World,1.f)},{{0,0,0},{1,0,0},0});
  step.view(view);
  require(strongest(view.near,&Memory::Cell::ledge).confidence==0 && strongest(view.near,&Memory::Cell::drop).confidence==0,
          "a step at the body's limit remains traversable");
  Memory::Pose limited = {{0,0,0},{1,0,0},1000};
  limited.stepHeightCm = 20;
  step.update({},limited);
  step.view(view);
  require(strongest(view.near,&Memory::Cell::ledge).confidence>0,"local step uses the actor's actual step height");

  Memory incline;
  incline.update({ray({1012,0,12},Kind::World,1.f),ray({1212,100,12},Kind::World,1.f)},{{0,0,0},{1,0,0},0});
  incline.view(view);
  require(strongest(view.far,&Memory::Cell::ledge).confidence==0,"a one-metre rise across two metres is walkable");
  limited.maxSlopeDegrees = 20;
  incline.update({},limited);
  incline.view(view);
  require(strongest(view.far,&Memory::Cell::ledge).confidence>0,"local slope uses the actor's actual walking limit");
  std::puts("local terrain: continuous slopes free; genuine step and drop retained; guild step/slope limits respected");
  }

void overheadWallAndWater() {
  Memory memory;
  Memory::View view;
  memory.update({ray({312,0,12},Kind::World,1.f),ray({312,300,12},Kind::World,0),
                 ray({512,0,12},Kind::Water)},{{0,0,0},{1,0,0},0});
  memory.view(view);
  require(strongest(view.near,&Memory::Cell::wall).confidence==0,"wall hit above the observed ground and body is overhead");
  require(strongest(view.near,&Memory::Cell::water).confidence>.99f,"water remains hazardous");
  memory.update({ray({312,0,12},Kind::World,1.f,1000),ray({312,150,12},Kind::World,0,1000),
                 ray({312,300,12},Kind::World,0,1000)},{{0,0,0},{1,0,0},1000});
  memory.view(view);
  require(strongest(view.near,&Memory::Cell::wall).confidence>.99f,"a higher wall hit cannot overwrite the observed wall in the body's space");

  Memory unseenGround;
  unseenGround.update({ray({312,300,12},Kind::World,0)},{{0,0,0},{1,0,0},0});
  unseenGround.view(view);
  require(strongest(view.near,&Memory::Cell::wall).confidence>.99f,"without observed local ground the wall remains conservative");
  std::puts("walls: observed overhead hit filtered; low wall and water retained; no guessed terrain under unseen wall");
  }

void windowAndTravelHistory() {
  Memory memory;
  Memory::View view;
  memory.update({ray({5000,150,0})},{{0,0,0},{1,0,0},0});
  // Distance must include the entire path, not just displacement or eye ticks.
  memory.update({},{{1000,0,0},{1,0,0},1000});
  memory.update({},{{0,0,0},{1,0,0},2000});
  memory.update({ray({5000,150,0})},{{0,0,0},{1,0,0},3000});
  memory.view(view);
  close(float(view.traveledCm),2000,1.e-4f,"empty observation ticks accumulate path");
  close(strongest(view.far,&Memory::Cell::wall).confidence,float(std::exp(-3./300.-2./30.)),1.e-5f,
        "returning to the same place does not restore confidence");
  memory.update({},{{20000,0,0},{1,0,0},4000});
  memory.update({},{{0,0,0},{1,0,0},5000});
  memory.view(view);
  require(strongest(view.far,&Memory::Cell::wall).confidence==0,"evicted world cells never reappear on returning");

  Memory late;
  late.update({},{{0,0,0},{1,0,0},0});
  late.update({},{{3000,0,0},{1,0,0},1000});
  late.update({ray({5000,150,0})},{{3000,0,0},{1,0,0},1000});
  late.view(view);
  close(strongest(view.far,&Memory::Cell::wall).confidence,float(std::exp(-1./300.-.1)),1.e-5f,
        "first ingestion of a cached observation uses its original path timestamp");
  std::puts("windows: world positions survive movement; eviction permanent; per-tick path and late cached observation correct");
  }

void farCellClearingNeedsActualIntersection() {
  Memory memory;
  Memory::View view;
  // Both projected lines cross the same 200 cm far cell at x=5000,z=0.
  // The second passes 150 cm beside the hit, at exactly the same height.
  memory.update({ray({5100,150,25},Kind::World,0,0,{0,150,25})},{{0,0,0},{1,0,0},0});
  memory.update({ray({5500,150,175},Kind::None,0,1000,{0,150,175})},{{0,0,0},{1,0,0},1000});
  memory.view(view);
  close(around(view.far,25,5100,&Memory::Cell::wall),float(std::exp(-1./300.)),1.e-5f,
        "parallel free ray 150 cm away in the same far cell does not erase a wall hit");
  memory.update({ray({5500,150,25},Kind::None,0,2000,{0,150,25})},{{0,0,0},{1,0,0},2000});
  memory.view(view);
  require(strongest(view.far,&Memory::Cell::wall).confidence==0,
          "free ray through the actual recorded hit clears it even in a far cell");
  std::puts("far clearing: parallel ray 150 cm away retains hit; ray through actual 3D hit clears it");
  }

void waterSurfaceIsNotEmptyAirOrSubmergedGround() {
  Memory memory;
  Memory::View view;
  memory.update({ray({512,0,12},Kind::Water)},{{0,0,0},{1,0,0},0});
  // This ray misses the water surface by ten centimetres. The generic free
  // clearance tolerance must not erase the static surface below that air.
  memory.update({ray({1000,10,12},Kind::None,0,100,{0,10,12})},{{0,0,0},{1,0,0},100});
  memory.view(view);
  require(around(view.near,12,512,&Memory::Cell::water)>.99f,
          "new free air above visible water does not make its surface traversable");
  memory.update({ray({512,-100,12},Kind::World,1.f,200)},{{0,0,0},{1,0,0},200});
  memory.view(view);
  require(around(view.near,12,512,&Memory::Cell::water)>.99f,
          "observing a submerged bottom does not replace visible water with free ground");
  std::puts("water: nearby empty air and observed submerged ground retain the water hazard");
  }

void boundedWaterPatches() {
  const auto sample = [](float height=0.f, float edge=60.f, uint64_t lastTime=0) {
    return std::vector<Ray>{ray({312,0,12},Kind::Water),ray({312+edge,0,12},Kind::Water),
                            ray({312,height,12+edge},Kind::Water,1.f,lastTime)};
    };
  const auto interior = [](const Memory::View& view)->const Memory::Cell& {
    // Centre of the near cell wholly between the three observed hit cells.
    return view.near.cells[45*Memory::Constants::nearSize+33]; // x=337.5,z=37.5
    };
  Memory memory;
  Memory::View view;
  memory.update(sample(),{{0,0,0},{1,0,0},0});
  memory.view(view);
  require(interior(view).water>.99f,"nearby simultaneous water hits bracket a continuous water surface");
  require(view.near.cells[47*Memory::Constants::nearSize+34].water==0,
          "water reconstruction does not extrapolate beyond its observed triangle");

  for(const auto& observations:{sample(0,150),sample(20),sample(0,60,100)}) {
    Memory isolated;
    isolated.update(observations,{{0,0,0},{1,0,0},100});
    isolated.view(view);
    require(interior(view).water==0,
            "distant, noncoplanar or asynchronous water hits do not fill unknown gaps");
    }

  for(float groundHeight:{50.f,-100.f}) {
    Memory shore;
    auto observations=sample();
    observations.push_back(ray({337,groundHeight,37},Kind::World,1.f));
    shore.update(observations,{{0,0,0},{1,0,0},0});
    shore.view(view);
    require((interior(view).water>0)==(groundHeight<0),
            "water patches respect observed dry ground while retaining submerged bottom water");
    }
  std::puts("water patches: bounded same-sample coplanar triangles; no dry-land, long-gap or height extrapolation");
  }

Memory::FamiliarMask allTiles(const Memory::FamiliarMap& map) {
  return Memory::FamiliarMask((map.tiles.size()+63)/64,~uint64_t(0));
  }

Memory::View uncachedFamiliarView(const Memory& memory, const Memory::FamiliarMap* map,
                                 const Memory::FamiliarMask& mask) {
  // Read live evidence in world-axis order, then apply the former per-cell
  // atlas lookup before rotating/splatting. No window cache supplies this oracle.
  Memory live=memory;
  live.setFamiliarMap(nullptr,{});
  Memory::View output, axis;
  live.view(output);
  auto pose=memory.pose();
  pose.forward={1,0,0};
  live.update({},pose);
  live.view(axis);
  const auto& body=memory.pose();
  const size_t size=Memory::Constants::farSize;
  const float cellCm=Memory::Constants::farCellCm;
  const auto coordinate=[&](float value) { return int(std::floor(value/cellCm)); };
  const int minX=coordinate(body.position.x)-int(size/2);
  const int minZ=coordinate(body.position.z)-int(size/2);
  const Vec3 right(-body.forward.z,0,body.forward.x);
  output.far.cells.assign(size*size,Memory::Cell{});
  for(int z=minZ; z<minZ+int(size); ++z)
    for(int x=minX; x<minX+int(size); ++x) {
      const float dx=(float(x)+.5f)*cellCm-body.position.x;
      const float dz=(float(z)+.5f)*cellCm-body.position.z;
      const int ax=coordinate(dz+float(size/2)*cellCm);
      const int az=coordinate(dx+float(size/2)*cellCm);
      require(ax>=0 && az>=0 && ax<int(size) && az<int(size),
              "uncached oracle poses preserve every axis-aligned world cell");
      auto cell=axis.far.cells[size_t(az)*size+size_t(ax)];
      if(map && !mask.empty() && cell.confidence<Memory::Constants::familiarConfidence) {
        const auto next=std::lower_bound(map->tiles.begin(),map->tiles.end(),std::pair(x,z),
                                        [](const auto& tile,const auto& key) {
          return std::pair(tile.x,tile.z)<key;
          });
        const float bodyHeight=std::isfinite(body.bodyHeightCm) && body.bodyHeightCm>0.f ?
                               body.bodyHeightCm : Memory::Constants::bodyHeightCm;
        if(next!=map->tiles.end() && next->x==x && next->z==z &&
           (mask[size_t(next-map->tiles.begin())/64]&(uint64_t(1)<<(size_t(next-map->tiles.begin())%64)))!=0 &&
           !(std::isfinite(cell.groundHeightCm) && std::isfinite(next->groundHeightCm) &&
             std::abs(cell.groundHeightCm-next->groundHeightCm)>bodyHeight)) {
          const float movable=cell.movable;
          cell=Memory::Cell{};
          cell.confidence=Memory::Constants::familiarConfidence;
          cell.familiar=1;
          cell.groundHeightCm=next->groundHeightCm;
          const auto value=[&](uint8_t channel) {
            return (next->channels&channel)!=0 ? Memory::Constants::familiarConfidence : 0.f;
            };
          cell.free=value(Memory::FamiliarTile::Free);
          cell.wall=value(Memory::FamiliarTile::Wall);
          cell.ledge=value(Memory::FamiliarTile::Ledge);
          cell.drop=value(Memory::FamiliarTile::Drop);
          cell.water=value(Memory::FamiliarTile::Water);
          cell.movable=movable;
          }
        }
      const int vx=coordinate(dx*right.x+dz*right.z+float(size/2)*cellCm);
      const int vz=coordinate(dx*body.forward.x+dz*body.forward.z+float(size/2)*cellCm);
      if(vx<0 || vz<0 || vx>=int(size) || vz>=int(size) || cell.confidence<=0.f)
        continue;
      auto& target=output.far.cells[size_t(vz)*size+size_t(vx)];
      if(target.confidence>0.f && target.familiar!=cell.familiar) {
        if(target.familiar==0.f && target.confidence>=Memory::Constants::familiarConfidence)
          continue;
        if(cell.familiar==0.f && cell.confidence>=Memory::Constants::familiarConfidence)
          target=Memory::Cell{};
        }
      for(Field field:{&Memory::Cell::confidence,&Memory::Cell::free,&Memory::Cell::wall,
                       &Memory::Cell::ledge,&Memory::Cell::drop,&Memory::Cell::water,
                       &Memory::Cell::movable,&Memory::Cell::familiar})
        target.*field=std::max(target.*field,cell.*field);
      if(std::isfinite(cell.groundHeightCm))
        target.groundHeightCm=cell.groundHeightCm;
      }
  return output;
  }

void familiarWindowCacheMatchesUncached() {
  Memory::FamiliarMap first;
  for(int x=-70; x<=70; ++x)
    for(int z=-70; z<=70; ++z)
      if((x*3+z*5)%7==0) {
        Memory::FamiliarTile tile;
        tile.x=x; tile.z=z;
        tile.channels=uint8_t(1+((x*x+z*z)%31));
        if((tile.channels&Memory::FamiliarTile::Free)!=0)
          tile.groundHeightCm=float((x+z)%3)*200.f;
        first.tiles.push_back(tile);
        }
  Memory::FamiliarTile rock;
  rock.x=25; rock.z=0;
  rock.channels=Memory::FamiliarTile::Free|Memory::FamiliarTile::Wall|Memory::FamiliarTile::Water;
  rock.groundHeightCm=0;
  first.tiles.push_back(rock);
  first.finish();
  Memory::FamiliarMap second;
  second.tiles=first.tiles;
  for(auto& tile:second.tiles)
    tile.channels=uint8_t(tile.channels^Memory::FamiliarTile::Wall);
  second.finish();
  auto mask=allTiles(first);
  const Memory::FamiliarMap* map=&first;
  Memory memory;
  memory.setFamiliarMap(map,mask);
  size_t checks=0;
  const auto check=[&] {
    Memory::View cached;
    memory.view(cached);
    const auto expected=uncachedFamiliarView(memory,map,mask);
    for(const auto& layers:{std::pair(&cached.near,&expected.near),std::pair(&cached.far,&expected.far)}) {
      require(layers.first->size==layers.second->size && layers.first->cellCm==layers.second->cellCm,
              "cache preserves layer dimensions");
      for(size_t i=0; i<layers.first->cells.size(); ++i) {
        const auto& actual=layers.first->cells[i];
        const auto& reference=layers.second->cells[i];
        for(Field field:{&Memory::Cell::confidence,&Memory::Cell::free,&Memory::Cell::wall,
                         &Memory::Cell::ledge,&Memory::Cell::drop,&Memory::Cell::water,
                         &Memory::Cell::movable,&Memory::Cell::familiar,&Memory::Cell::groundHeightCm})
          require(actual.*field==reference.*field ||
                  (std::isnan(actual.*field) && std::isnan(reference.*field)),
                  "cached familiar window equals uncached lookup for every memory channel");
        }
      }
    ++checks;
    };
  uint64_t time=0;
  for(const auto& position:std::vector<Vec3>{{0,0,0},{50,0,50},{250,0,-150},{600,0,600},
                                          {-400,0,200},{16000,0,-16000},{0,0,0}})
    for(const auto& forward:std::vector<Vec3>{{1,0,0},{1,0,1},{-.8f,0,.6f},{0,0,-1}}) {
      memory.update({}, {position,forward,time++});
      check();
      }
  memory.update({ray({5500,150,25},Kind::None,0,100)},{{0,0,0},{1,0,1},100});
  check();
  memory.update({},{{0,0,0},{1,0,1},400000});
  check();
  memory.update({ray({5100,600,25},Kind::World,1.f,400100)},{{0,0,0},{1,0,1},400100});
  memory.update({},{{0,0,0},{1,0,1},800000});
  check();
  // The same window must refresh for a changed map or personal tile mask.
  map=&second;
  memory.setFamiliarMap(map,mask);
  check();
  for(size_t i=0; i<mask.size(); ++i)
    mask[i]=i%2 ? 0x5555555555555555ull : 0xaaaaaaaaaaaaaaaaull;
  memory.setFamiliarMap(map,mask);
  check();
  memory.update({},{{600,0,200},{1,0,1},800100});
  check();
  memory.reset();
  memory.update({},{{0,0,0},{1,0,1},0});
  check();
  mask.clear();
  memory.setFamiliarMap(map,mask);
  check();
  mask=allTiles(second);
  memory.setFamiliarMap(map,mask);
  check();
  map=nullptr;
  mask.clear();
  memory.setFamiliarMap(map,mask);
  check();
  std::printf("familiar cache: %zu exact cached/uncached near+far comparisons across motion, rotation, masks, maps and live precedence\n",checks);
  }

void journeyMemoryLastsMinutes() {
  Memory memory;
  Memory baseline;
  baseline.setJourneyMemory(false);
  Memory::View view;
  for(auto* current:{&memory,&baseline}) {
    current->update({ray({5000,150,0})},{{0,0,0},{1,0,0},0});
    current->update({},{{7500,0,0},{1,0,0},60000});
    current->update({},{{0,0,0},{1,0,0},120000});
    }
  memory.view(view);
  const float confidence=strongest(view.far,&Memory::Cell::wall).confidence;
  close(confidence,float(std::exp(-120./300.-150./300.)),1.e-5f,
        "rock seen two minutes and 150 travelled metres ago is still remembered");
  require(confidence>.4f,"journey rock retains useful confidence after two minutes and 150 metres");
  baseline.view(view);
  close(strongest(view.far,&Memory::Cell::wall).confidence,float(std::exp(-120./30.-150./30.)),1.e-6f,
        "comparison switch preserves v3 static decay");
  std::printf("journey: after 120 s / 150 m rock confidence %.6f\n",confidence);
  }

void familiarMapHonestyAndLivePriority() {
  Memory observed;
  observed.update({ray({5100,150,25})},{{0,0,0},{1,0,0},0});
  Memory::FamiliarMap map;
  map.bake(observed,17);
  map.finish();
  require(!map.tiles.empty(),"eye observations create shared familiar tiles");
  require(sizeof(Memory::FamiliarTile)==24,"familiar shared tile remains compact");

  Memory stranger;
  Memory local;
  local.setFamiliarMap(&map,allTiles(map));
  Memory maskedStranger;
  maskedStranger.setFamiliarMap(&map,{});
  Memory::View view;
  for(auto* current:{&stranger,&maskedStranger,&local})
    current->update({},{{0,0,0},{1,0,0},1000});
  for(auto* current:{&stranger,&maskedStranger}) {
    current->view(view);
    require(strongest(view.far,&Memory::Cell::wall).confidence==0,
            "stranger has no unseen rock, including when a shared map exists without a mask");
    }
  local.view(view);
  close(strongest(view.far,&Memory::Cell::wall).confidence,Memory::Constants::familiarConfidence,1.e-6f,
        "local has a familiar rock in far map before seeing it");
  require(strongest(view.far,&Memory::Cell::familiar).confidence==1,
          "familiar fallback has a separate source channel");
  require(strongest(view.near,&Memory::Cell::familiar).confidence==0,"familiar map never fills near memory");
  local.reset();
  local.update({},{{0,0,0},{1,0,0},1000});
  local.view(view);
  close(strongest(view.far,&Memory::Cell::wall).confidence,Memory::Constants::familiarConfidence,1.e-6f,
        "resetting live eye observations preserves local familiar knowledge");
  stranger.update({ray({5100,150,25},Kind::World,0,1100)},{{0,0,0},{1,0,0},1100});
  stranger.view(view);
  require(strongest(view.far,&Memory::Cell::wall).confidence>.99f,
          "stranger remembers the rock after its own eye observes it");

  local.update({ray({5500,150,25},Kind::None,0,1200,{0,150,25})},{{0,0,0},{1,0,0},1200});
  local.view(view);
  require(around(view.far,25,5100,&Memory::Cell::wall)==0,
          "fresh observed passage overrides familiar wall");
  require(around(view.far,25,5100,&Memory::Cell::familiar)==0,
          "fresh partial free channel prevents every familiar channel in its cell");
  local.update({},{{0,0,0},{1,0,0},400000});
  local.view(view);
  close(around(view.far,25,5100,&Memory::Cell::wall),Memory::Constants::familiarConfidence,1.e-6f,
        "familiar map returns only after live evidence is older than the fallback threshold");
  std::printf("familiar: %zu shared tiles / %zu bytes, stranger unknown, local .35, fresh passage wins\n",
              map.tiles.size(),map.storageBytes());
  }

void movableIsNeverLongTermKnowledge() {
  Memory observed;
  observed.update({ray({5100,150,25},Kind::MovableVob)},{{0,0,0},{1,0,0},0});
  Memory::FamiliarMap map;
  map.bake(observed);
  map.finish();
  Memory local;
  local.setFamiliarMap(&map,allTiles(map));
  local.update({},{{0,0,0},{1,0,0},60000});
  Memory::View view;
  local.view(view);
  require(strongest(view.far,&Memory::Cell::movable).confidence==0,"movable is never baked into familiar map");
  observed.update({},{{0,0,0},{1,0,0},60000});
  observed.view(view);
  close(strongest(view.far,&Memory::Cell::movable).confidence,float(std::exp(-60./5.)),1.e-7f,
        "movable retains five-second decay despite longer journey memory");
  std::puts("movable: familiar map excludes it, live confidence after 60 s is below .00001");
  }

void familiarSourcesDoNotShareHiddenChannels() {
  Memory wall;
  wall.update({ray({5100,150,25})},{{0,0,0},{1,0,0},0});
  Memory water;
  water.update({ray({5100,0,25},Kind::Water)},{{0,0,0},{1,0,0},0});
  Memory::FamiliarMap map;
  map.bake(wall,3);
  map.bake(water,4);
  map.finish();
  Memory::FamiliarMask mask((map.tiles.size()+63)/64,0);
  for(size_t i=0; i<map.tiles.size(); ++i) {
    if(map.tiles[i].source==3)
      mask[i/64]|=uint64_t(1)<<(i%64);
    if(map.tiles[i].x==25 && map.tiles[i].z==0) {
      require(map.tiles[i].source==3,"equal canonical sources use stable lowest source id");
      require((map.tiles[i].channels&Memory::FamiliarTile::Water)==0,
              "another source's water cannot leak into the canonical wall tile");
      }
    }
  Memory local;
  local.setFamiliarMap(&map,std::move(mask));
  local.update({},{{0,0,0},{1,0,0},0});
  Memory::View view;
  local.view(view);
  require(strongest(view.far,&Memory::Cell::water).confidence==0,
          "local cannot read a hidden classification supplied by another source pose");
  std::puts("familiar provenance: canonical tiles never union hidden observations from another source");
  }
}

int main() {
  turnHonestyAndFreeSpace();
  timeAndDistanceDecay();
  cachedRaysAndIncompleteRays();
  groundAndLowEmptyAir();
  localStepsAndSlopes();
  overheadWallAndWater();
  windowAndTravelHistory();
  farCellClearingNeedsActualIntersection();
  waterSurfaceIsNotEmptyAirOrSubmergedGround();
  boundedWaterPatches();
  familiarWindowCacheMatchesUncached();
  journeyMemoryLastsMinutes();
  familiarMapHonestyAndLivePriority();
  movableIsNeverLongTermKnowledge();
  familiarSourcesDoNotShareHiddenChannels();
  std::puts("navigation memory contracts passed");
  }

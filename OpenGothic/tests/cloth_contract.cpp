#include "asset/fromzenkit.h"
#include <zenkit/world/BspTree.hh>
#include <zenkit/Mesh.hh>
#include "physics/cloth.h"
#include "physics/collisionworld.h"
#include "physics/dynamicworld.h"
#include "physics/worldwind.h"
#include "world/portalrooms.h"

#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/SoftBody/SoftBodyMotionProperties.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

// The production engine may ask Jolt for extra collision steps to support
// unrelated rigid bodies. Exercise those real Update paths without adding a
// fragment, NPC or test-mode switch to the game itself.
class ClothContractDriver {
  public:
    static JPH::PhysicsSystem& system(CollisionWorld& world) { return world.physicsSystem(); }
    static std::array<Cloth::Snapshot,2> run(const std::vector<Tempest::Vec3>& vertices,
                                            const std::vector<uint32_t>& indices,
                                            const std::vector<uint32_t>& pins,
                                            uint32_t collisionSteps) {
      CollisionWorld world;
      Cloth cloth(world);
      auto instance=cloth.add(vertices,indices,pins,Tempest::Matrix4x4::mkIdentity(),.82f,"substep-invariance",18);
      cloth.setObserver({350,350,0});
      cloth.setWindScale(2.f);
      JPH::TempAllocatorImpl allocator(16*1024*1024);
      JPH::JobSystemSingleThreaded jobs(4096);
      std::array<Cloth::Snapshot,2> result;
      float accumulated=0;
      uint64_t now=0;
      bool calm=false;
      for(;now<13000;now+=17) {
        if(!calm && now>=3000) {
          result[0]=cloth.snapshots().front();
          cloth.setWindScale(0.f);
          calm=true;
          }
        cloth.setWind(WorldWind::sample(now,10000,true));
        cloth.prepareFrame(17);
        constexpr float step=1.f/60.f;
        accumulated+=.017f;
        const int count=int(accumulated/step);
        accumulated-=float(count)*step;
        for(int i=0;i<std::min(count,2);++i) {
          world.physicsSystem().Update(step,int(collisionSteps),&allocator,&jobs);
          }
        cloth.finishFrame();
        }
      result[1]=cloth.snapshots().front();
      return result;
      }
  };

namespace {
  void require(bool condition,const char* message) {
    if(condition) return;
    std::fprintf(stderr,"FAIL: %s\n",message);
    std::exit(EXIT_FAILURE);
    }
  struct Flag {
    std::vector<Tempest::Vec3> vertices;
    std::vector<uint32_t> indices,pins;
    Flag() {
      constexpr uint32_t width=9,height=13;
      for(uint32_t y=0;y<height;++y)
        for(uint32_t x=0;x<width;++x) {
          vertices.emplace_back(float(x)*15.f,200.f-float(y)*15.f,0.f);
          if(y==0) pins.push_back(uint32_t(vertices.size()-1));
          }
      for(uint32_t y=0;y+1<height;++y)
        for(uint32_t x=0;x+1<width;++x) {
          const uint32_t a=y*width+x,b=a+1,c=a+width,d=c+1;
          indices.insert(indices.end(),{a,b,c,b,d,c});
          }
      }
    };
  struct LongFlag {
    std::vector<Tempest::Vec3> vertices;
    std::vector<uint32_t> indices,pins;
    explicit LongFlag(uint32_t grid=17) {
      const uint32_t width=grid,height=grid;
      for(uint32_t y=0;y<height;++y)
        for(uint32_t x=0;x<width;++x) {
          const float u=float(x)/float(width-1),v=float(y)/float(height-1);
          vertices.emplace_back(730.f*u,500.f-335.f*v,30.f*std::sin(u*3.14159265f));
          if(x==0) pins.push_back(uint32_t(vertices.size()-1));
          }
      for(uint32_t y=0;y+1<height;++y)
        for(uint32_t x=0;x+1<width;++x) {
          const uint32_t a=y*width+x,b=a+1,c=a+width,d=c+1;
          indices.insert(indices.end(),{a,b,c,b,d,c});
          }
      }
    };
  struct RoomFixture {
    zenkit::Mesh mesh;
    zenkit::BspTree bsp;
    RoomFixture() {
      const auto quad=[&](zenkit::Vec3 a,zenkit::Vec3 b,zenkit::Vec3 c,zenkit::Vec3 d,bool portal=false,bool sector=true) {
        zenkit::Polygon polygon={};
        polygon.index_count=4;
        polygon.index_offset=mesh.polygon_vertex_indices.size();
        polygon.flags.is_sector=sector;
        polygon.flags.is_portal=portal;
        polygon.flags.sector_index=-1; // Stock worlds do not populate this.
        for(const auto& vertex:{a,b,c,d}) {
          mesh.polygon_vertex_indices.push_back(uint32_t(mesh.vertices.size()));
          mesh.vertices.push_back(vertex);
          }
        mesh.geometry.push_back(polygon);
        };
      quad({-400,-100,-400},{400,-100,-400},{400,-100,400},{-400,-100,400});
      quad({-400,450,-400},{-400,450,400},{400,450,400},{400,450,-400});
      quad({-400,-100,-400},{-400,450,-400},{400,450,-400},{400,-100,-400});
      quad({-400,-100,-400},{-400,-100,400},{-400,450,400},{-400,450,-400});
      quad({400,-100,-400},{400,450,-400},{400,450,400},{400,-100,400});
      quad({-400,-100,400},{400,-100,400},{400,450,400},{-400,450,400},true,false);
      // A wide yard in the same broad BSP leaf is ordinary world geometry.
      quad({-500,800,-500},{1500,800,-500},{1500,800,500},{-500,800,500},false,false);
      bsp.mode=zenkit::BspTreeType::OUTDOOR;
      zenkit::BspNode leaf={};
      leaf.bbox.min={-10000,-10000,-10000};
      leaf.bbox.max={10000,10000,10000};
      leaf.polygon_index=0;
      leaf.polygon_count=uint32_t(mesh.geometry.size());
      bsp.nodes.push_back(leaf);
      bsp.leaf_node_indices.push_back(0);
      for(uint32_t i=0;i<mesh.geometry.size();++i) bsp.polygon_indices.push_back(i);
      zenkit::BspSector room;
      room.name="HALL";
      room.node_indices={0};
      room.portal_polygon_indices={5};
      bsp.sectors.push_back(std::move(room));
      }
    };
  void tick(CollisionWorld& world,size_t count) { for(size_t i=0;i<count;++i) world.tick(17); }
  float lateralSpeed(const Cloth::Snapshot& from,const Cloth::Snapshot& to) {
    float speed=0;
    for(size_t i=0;i<from.worldPositionsCm.size();++i) {
      const auto delta=to.worldPositionsCm[i]-from.worldPositionsCm[i];
      speed=std::max(speed,std::sqrt(delta.x*delta.x+delta.z*delta.z)/.017f);
      }
    return speed;
    }
  float maxDisplacement(const Cloth::Snapshot& from,const Cloth::Snapshot& to) {
    float distance=0;
    for(size_t i=0;i<from.worldPositionsCm.size();++i)
      distance=std::max(distance,(to.worldPositionsCm[i]-from.worldPositionsCm[i]).length());
    return distance;
    }
  Cloth::Snapshot fixedCadence(const Flag& flag,uint64_t frameMs) {
    CollisionWorld world;
    Cloth cloth(world);
    auto instance=cloth.add(flag.vertices,flag.indices,flag.pins,Tempest::Matrix4x4::mkIdentity(),.85f,"fixed-cadence",19);
    cloth.setObserver({60,100,0});
    cloth.setWind({4,0,1});
    for(uint64_t now=0;now<1000;now+=frameMs) world.tick(frameMs);
    return cloth.snapshots().front();
    }
  std::vector<Tempest::Vec3> nativeVelocities(CollisionWorld& world) {
    JPH::BodyIDVector bodies;
    auto& system=ClothContractDriver::system(world);
    system.GetBodies(bodies);
    require(bodies.size()==1,"the isolated calm fixture owns exactly one native body");
    JPH::BodyLockRead lock(system.GetBodyLockInterface(),bodies.front());
    require(lock.Succeeded() && lock.GetBody().GetBodyType()==JPH::EBodyType::SoftBody,
            "the coarse flag retains a registered native soft body outside broadphase");
    const auto& motion=*static_cast<const JPH::SoftBodyMotionProperties*>(lock.GetBody().GetMotionProperties());
    std::vector<Tempest::Vec3> result;
    for(const auto& vertex:motion.GetVertices())
      result.emplace_back(vertex.mVelocity.GetX(),vertex.mVelocity.GetY(),vertex.mVelocity.GetZ());
    return result;
    }
  Cloth::Snapshot coarseFlagCalm(const LongFlag& flag,float damping,bool freeze) {
    CollisionWorld world;
    Cloth cloth(world);
    auto instance=cloth.add(flag.vertices,flag.indices,flag.pins,Tempest::Matrix4x4::mkIdentity(),.82f,"coarse-long-flag-calm",21);
    instance->setLinearDamping(damping);
    bool visible=true;
    cloth.setVisibilityTest([&](const Tempest::Vec3&,float) { return visible; });
    cloth.setObserver({350,350,0});
    cloth.setWindScale(2.f);
    uint64_t now=0;
    const auto advance=[&](uint64_t durationMs) {
      for(uint64_t elapsed=0;elapsed<durationMs;elapsed+=10) {
        cloth.setWind(WorldWind::sample(now,10000,true));
        world.tick(10);
        now+=10;
        }
      };
    advance(3000);
    const auto gust=cloth.snapshots().front();
    require(gust.vertices==81 && gust.pinned==9,"coarse seven-metre flag uses eighty-one physical particles and a fixed side edge");
    require(gust.visible && gust.active && gust.simulationWeight==1.f && gust.simulationSteps==90,
            "three exact seconds of visible gusts advance ninety cloth steps");
    require(gust.maxSpeedEverCmPerSec<=Cloth::maxVelocityMS*100.f+1.f,"coarse long-flag gusts obey the native vertex speed cap");
    if(freeze) {
      require(gust.maxSpeedCmPerSec>1.f,"the freeze comparison starts while the coarse flag is still moving");
      const auto movingVelocities=nativeVelocities(world);
      visible=false;
      world.tick(0);
      advance(2000);
      const auto hidden=cloth.snapshots().front();
      require(!hidden.visible && hidden.active && hidden.simulationSteps==gust.simulationSteps,
              "two hidden seconds preserve the coarse flag without native steps");
      require(maxDisplacement(gust,hidden)<.0001f && hidden.maxSpeedCmPerSec==gust.maxSpeedCmPerSec,
              "hidden time preserves both the moving pose and its recorded native velocity");
      const auto hiddenVelocities=nativeVelocities(world);
      require(movingVelocities.size()==81 && hiddenVelocities.size()==movingVelocities.size(),"hidden time preserves the coarse native particle array");
      for(size_t i=0;i<movingVelocities.size();++i)
        require((movingVelocities[i]-hiddenVelocities[i]).quadLength()==0.f,"hiding moving cloth never resets or advances its native vertex velocities");
      visible=true;
      world.tick(0);
      const auto returned=cloth.snapshots().front();
      require(returned.visible && returned.simulationSteps==gust.simulationSteps && maxDisplacement(hidden,returned)<.0001f,
              "a moving coarse flag returns without a pose jump or catch-up steps");
      }
    cloth.setWindScale(0.f);
    const auto calmStartSteps=cloth.snapshots().front().simulationSteps;
    advance(10000);
    const auto calm=cloth.snapshots().front();
    std::printf("coarse cloth calm: damping=%.2f hidden_ms=%u visible_calm_ms=10000 calm_steps=%llu max_vertex_cm_s=%.6g anchor_drift_cm=%.6g recovery=%llu\n",
                double(damping),freeze ? 2000u : 0u,static_cast<unsigned long long>(calm.simulationSteps-calmStartSteps),
                double(calm.maxSpeedCmPerSec),double(calm.maxAnchorErrorCm),static_cast<unsigned long long>(calm.recoveryCount));
    require(calm.visible && calm.active && calm.simulationWeight==1.f && calm.simulationSteps-calmStartSteps==300,
            "ten exact seconds of visible calm advance three hundred coarse cloth steps");
    // This broadside synthetic sheet retains about 2.82 cm/s in the original
    // 60 Hz / eight-substep reference. A 3 cm/s guard preserves that material's
    // calm response while catching the doubled-XPBD-step regression (~4.86).
    // Authored ship geometry is also measured separately by the live fixture.
    require(calm.maxSpeedCmPerSec<3.f,"a coarse seven-metre flag preserves the original calm response below three centimetres per second");
    require(calm.maxAnchorErrorCm<=.5f && calm.recoveryCount==0,"coarse long flags stay attached and finite through gusts and calm");
    return calm;
    }
  }

int main() {
  constexpr uint64_t windPeriod=10000;
  const auto windBefore=WorldWind::sample(windPeriod-1,windPeriod,true);
  const auto windAfter=WorldWind::sample(windPeriod+1,windPeriod,true);
  require((windAfter-windBefore).length()<.02f,"world wind has no discontinuity at a shader cycle boundary");
  require(WorldWind::sample(23456,windPeriod,false).quadLength()==0.f,"disabled world wind is exactly zero");
  require(WorldWind::sample(23456,windPeriod,true,0).quadLength()==0.f,"calm world wind is exactly zero");
  require(WorldWind::sample(23456,0,true).quadLength()==0.f,"zero wind period is exactly calm");
  require((WorldWind::sample(0,windPeriod,true)-WorldWind::sample(30000,windPeriod,true)).length()>.1f,"world wind actually changes over time");
  Flag cadenceFlag;
  const auto cadenceReference=fixedCadence(cadenceFlag,10);
  require(cadenceReference.simulationSteps==30,"visible cloth advances exactly thirty fixed steps in one second");
  float cadenceDelta=0;
  for(uint64_t frameMs:{5u,20u,40u}) {
    const auto cadence=fixedCadence(cadenceFlag,frameMs);
    require(cadence.simulationSteps==30,"cloth's thirty-hertz cadence is independent of rendering frequency");
    cadenceDelta=std::max(cadenceDelta,maxDisplacement(cadenceReference,cadence));
    require(cadence.maxAnchorErrorCm<=.5f && cadence.recoveryCount==0,"all rendering frequencies preserve fixed pins and finite simulation");
    }
  require(cadenceDelta<.01f,"equal fixed steps give the same visible pose at different rendering frequencies");
  {
    CollisionWorld visibilityWorld;
    Cloth visibilityCloth(visibilityWorld);
    auto visibilityInstance=visibilityCloth.add(cadenceFlag.vertices,cadenceFlag.indices,cadenceFlag.pins,
                                               Tempest::Matrix4x4::mkIdentity(),.85f,"visibility-contract",20);
    visibilityInstance->setShaderReference([](uint32_t,const Tempest::Vec3& local,const Tempest::Vec3&) {
      return local+Tempest::Vec3(0,0,5);
      });
    bool visible=true;
    visibilityCloth.setVisibilityTest([&](const Tempest::Vec3&,float radius) {
      require(radius>0.f,"visibility query encloses the complete cloth");
      return visible;
      });
    visibilityCloth.setObserver({60,100,0});
    visibilityCloth.setWind({4,0,1});
    tick(visibilityWorld,60);
    auto interpolationStart=visibilityCloth.snapshots().front();
    require(interpolationStart.simulationWeight==1.f && interpolationStart.visible,"interpolation comparison starts fully simulated and visible");
    const auto previousSteps=interpolationStart.simulationSteps;
    // Locate a fixed-step boundary with less than one millisecond residual.
    for(size_t i=0;i<34 && interpolationStart.simulationSteps==previousSteps;++i) {
      visibilityWorld.tick(1);
      interpolationStart=visibilityCloth.snapshots().front();
      }
    require(interpolationStart.simulationSteps==previousSteps+1,"a visible fixed step occurs within one thirty-hertz period");
    visibilityWorld.tick(5);
    const auto interpolationMiddle=visibilityCloth.snapshots().front();
    visibilityWorld.tick(5);
    const auto interpolationEnd=visibilityCloth.snapshots().front();
    require(interpolationStart.simulationSteps==interpolationMiddle.simulationSteps &&
            interpolationStart.simulationSteps==interpolationEnd.simulationSteps,
            "render interpolation advances without an extra native soft-body step");
    float interpolationError=0;
    for(size_t i=0;i<interpolationEnd.positionsCm.size();++i) {
      const auto first=interpolationMiddle.positionsCm[i]-interpolationStart.positionsCm[i];
      const auto second=interpolationEnd.positionsCm[i]-interpolationMiddle.positionsCm[i];
      interpolationError=std::max(interpolationError,(first-second).length());
      }
    require(maxDisplacement(interpolationStart,interpolationEnd)>.001f,"rendered cloth keeps moving between fixed simulation steps");
    require(interpolationError<.001f,"equal render intervals interpolate equal distances between native poses");
    visible=false;
    visibilityWorld.tick(0);
    const auto hiddenStart=visibilityCloth.snapshots().front();
    require(!hiddenStart.visible && hiddenStart.active,"hidden cloth retains its transient native body");
    require(maxDisplacement(interpolationEnd,hiddenStart)<.0001f,"leaving the view freezes the last rendered pose immediately");
    for(size_t i=0;i<200;++i) visibilityWorld.tick(10);
    const auto hiddenEnd=visibilityCloth.snapshots().front();
    require(hiddenEnd.simulationSteps==hiddenStart.simulationSteps,"hidden cloth does no simulation work over two seconds");
    require(maxDisplacement(hiddenStart,hiddenEnd)<.0001f,"hidden cloth preserves its last visible pose");
    visible=true;
    visibilityWorld.tick(0);
    const auto returned=visibilityCloth.snapshots().front();
    require(returned.visible && returned.simulationSteps==hiddenEnd.simulationSteps,"returning cloth starts without catch-up simulation");
    require(maxDisplacement(hiddenEnd,returned)<.0001f,"returning to the camera preserves the last pose without a jump");
    visibilityWorld.tick(10);
    const auto resumed=visibilityCloth.snapshots().front();
    require(resumed.simulationSteps==returned.simulationSteps,"a hidden interval never accumulates simulation debt");
    require(maxDisplacement(returned,resumed)<=Cloth::maxVelocityMS*100.f*.010f+.01f,
            "the first resumed render interval stays within the native speed bound");
    visible=false;
    visibilityWorld.tick(0);
    visibilityCloth.setEnabled(false);
    for(size_t i=0;i<60;++i) visibilityWorld.tick(10);
    const auto hiddenOff=visibilityCloth.snapshots().front();
    require(!hiddenOff.visible && !hiddenOff.active && hiddenOff.simulationWeight==0.f,"cloth OFF finishes the shader transition while hidden");
    for(size_t i=0;i<cadenceFlag.vertices.size();++i)
      require((hiddenOff.positionsCm[i]-cadenceFlag.vertices[i]-Tempest::Vec3(0,0,5)).length()<.001f,
              "hidden cloth OFF restores the exact shader reference");
    }
  CollisionWorld collision;
  Cloth cloth(collision);
  Flag flag;
  auto instance=cloth.add(flag.vertices,flag.indices,flag.pins,Tempest::Matrix4x4::mkIdentity(),.85f,"contract-flag",7);
  require(instance!=nullptr,"valid cloth registers");
  require(!instance->active(),"registration alone creates no native soft body");
  cloth.setObserver({60,100,0});
  tick(collision,600);
  auto calm=cloth.snapshots().front();
  require(calm.active && calm.simulationWeight==1.f,"near cloth becomes fully simulated");
  require(calm.vertices==flag.vertices.size() && calm.pinned==flag.pins.size(),"soft mesh preserves visible vertices and pins");
  require(calm.maxAnchorErrorCm<=.5f,"pinned vertices stay within half a centimetre");
  require(calm.recoveryCount==0,"calm cloth never needs recovery");
  require(std::isfinite(calm.maxSpeedCmPerSec),"calm velocity remains finite");
  cloth.setWind({4,0,1});
  tick(collision,600);
  auto windy=cloth.snapshots().front();
  require(windy.maxSpeedEverCmPerSec<=Cloth::maxVelocityMS*100.f+1.f,"native vertex speed cap bounds gusts");
  require(windy.recoveryCount==0,"gusts never explode or fly away");
  require(windy.maxAnchorErrorCm<=.5f,"gusts preserve attachments");
  const auto& vertices=windy.worldPositionsCm;
  const uint32_t a=flag.indices[flag.indices.size()/2],b=flag.indices[flag.indices.size()/2+1],c=flag.indices[flag.indices.size()/2+2];
  const auto center=(vertices[a]+vertices[b]+vertices[c])*(1.f/3.f);
  auto normal=Tempest::Vec3::crossProduct(vertices[b]-vertices[a],vertices[c]-vertices[a]);
  normal=Tempest::Vec3::normalize(normal);
  require(cloth.projectile(center+normal*100.f,center-normal*100.f,normal*.35f,true)==1,"arrow crosses real deformed triangles");
  require(cloth.projectile(center+normal*100.f,center-normal*100.f,normal*5.f,false)==1,"fireball pushes real deformed triangles");
  auto hit=cloth.snapshots().front();
  require(hit.arrowPasses==1 && hit.projectileHits==2,"one pass-through arrow and one spell are counted");
  constexpr uint32_t solidMask=(1u<<DynamicWorld::C_Landscape)|(1u<<DynamicWorld::C_Object)|(1u<<DynamicWorld::C_ActiveBody);
  require(!collision.rayCast(center+normal*100.f,center-normal*100.f,solidMask,true).hasHit,"cloth never blocks solid or character queries");
  require(!collision.rayCast(center+normal*100.f,center-normal*100.f,~0u,true).hasHit,"even an all-layer gameplay ray passes through cloth");
  tick(collision,120);
  instance->setShaderReference([](uint32_t,const Tempest::Vec3& local,const Tempest::Vec3&) { return local+Tempest::Vec3(0,0,5); });
  // Walk through the transition band with fixed geometry and a stationary
  // shader phase: only cloth simulation and the fallback transition can move it.
  for(float distance=3450.f;distance<=3995.f;distance+=5.f) {
    cloth.setObserver({60.f+distance,110,0});
    collision.tick(17);
    }
  cloth.setObserver({4059,110,0}); // one centimetre inside the 40 m radius
  collision.tick(17);
  const auto boundaryBefore=cloth.snapshots().front();
  cloth.setObserver({4061,110,0}); // one centimetre outside
  collision.tick(17);
  const auto boundaryAfter=cloth.snapshots().front();
  const float boundaryDelta=maxDisplacement(boundaryBefore,boundaryAfter);
  require(boundaryDelta<=.5f,"rendered cloth crosses the radius without a half-centimetre pop");
  require(boundaryAfter.simulationWeight==0.f && !boundaryAfter.active,"crossing the radius reaches shader fallback");
  for(size_t i=0;i<flag.vertices.size();++i)
    require((boundaryAfter.positionsCm[i]-flag.vertices[i]-Tempest::Vec3(0,0,5)).length()<.001f,"outside radius visible vertices match the shader phase");
  cloth.setObserver({10000,100,0});
  tick(collision,40);
  require(!instance->active() && cloth.bodyCount()==0,"distant cloth releases native soft body");
  cloth.setObserver({60,100,0});
  tick(collision,40);
  require(instance->active(),"returning observer recreates transient cloth");
  size_t updateCalls=0;
  bool callbackEnabled=true;
  float callbackWeight=instance->simulationWeight();
  instance->setUpdateCallback([&](const std::vector<Tempest::Vec3>& positions,float weight) {
    require(positions.size()==flag.vertices.size(),"toggle callback receives complete visible geometry");
    ++updateCalls;
    callbackEnabled=cloth.enabled();
    callbackWeight=weight;
    });
  const float weightBeforeDisable=instance->simulationWeight();
  cloth.setEnabled(false);
  require(updateCalls==1 && !callbackEnabled && callbackWeight==weightBeforeDisable,
          "cloth OFF synchronizes render callbacks immediately without advancing physics");
  tick(collision,40);
  require(!instance->active() && cloth.bodyCount()==0,"cloth OFF returns entirely to shader");
  const auto callsBeforeEnable=updateCalls;
  cloth.setEnabled(true);
  require(updateCalls==callsBeforeEnable+1 && callbackEnabled && callbackWeight==0.f,
          "cloth ON restores fallback visibility before the next physics tick");
  tick(collision,40);
  require(instance->active(),"cloth ON recreates Jolt simulation");
  instance.reset();
  require(cloth.bodyCount()==0 && cloth.snapshots().empty(),"visual RAII releases native body");
  RoomFixture roomFixture;
  PortalRooms::Volumes rooms;
  rooms.reset(static_cast<asset::BspTreeType>(roomFixture.bsp.mode),asset::roomVolumesFromZenKit(roomFixture.mesh,roomFixture.bsp));
  require(rooms.isIndoor({60,100,0}),"authored room surfaces enclose an indoor point");
  require(rooms.isIndoor({60,100,350}),"an authored portal cap closes the room at its door");
  require(!rooms.isIndoor({60,100,450}),"a point beyond the portal door is outdoors");
  require(!rooms.isIndoor({60,700,0}),"a room's BSP leaf reaching the sky does not shelter a flag above its ceiling");
  require(!rooms.isIndoor({500,150,0}),"a wide world-mesh yard outside room surfaces does not create a room");
  roomFixture.bsp.mode=zenkit::BspTreeType::INDOOR;
  rooms.reset(static_cast<asset::BspTreeType>(roomFixture.bsp.mode),asset::roomVolumesFromZenKit(roomFixture.mesh,roomFixture.bsp));
  require(rooms.isIndoor({500,150,0}),"an entirely indoor BSP world has no outdoor wind");
  roomFixture.bsp.mode=zenkit::BspTreeType::OUTDOOR;
  rooms.reset(static_cast<asset::BspTreeType>(roomFixture.bsp.mode),asset::roomVolumesFromZenKit(roomFixture.mesh,roomFixture.bsp));
  cloth.setExposureTest([&](const Tempest::Vec3& point) { return !rooms.isIndoor(point); });
  auto roofMatrix=Tempest::Matrix4x4::mkIdentity();
  roofMatrix.translate(60,500,0);
  auto roof=collision.addCollisionBody(new JPH::BoxShape(JPH::Vec3(4,.1f,4)),roofMatrix,.7f,DynamicWorld::C_Object);
  auto sheltered=cloth.add(flag.vertices,flag.indices,flag.pins,Tempest::Matrix4x4::mkIdentity(),.85f,"sheltered-flag",8);
  require(roof!=nullptr && sheltered!=nullptr,"roof and sheltered cloth register");
  cloth.setObserver({60,100,0});
  cloth.setWind({0,0,0});
  tick(collision,600);
  const auto roofCalmBefore=cloth.snapshots().front();
  collision.tick(17);
  const auto roofCalmAfter=cloth.snapshots().front();
  require(!roofCalmAfter.exposed,"cloth in the actual authored hall is sheltered from world wind");
  const float calmLateral=lateralSpeed(roofCalmBefore,roofCalmAfter);
  cloth.setWind({8,0,4});
  tick(collision,600);
  const auto roofWindBefore=cloth.snapshots().front();
  collision.tick(17);
  const auto roofWindAfter=cloth.snapshots().front();
  const float shelteredLateral=lateralSpeed(roofWindBefore,roofWindAfter);
  require(!roofWindAfter.exposed,"indoor shelter persists through strong wind");
  require(shelteredLateral<=calmLateral+.25f,"strong world wind adds no lateral motion under a roof");
  sheltered.reset();
  roof.reset();
  const std::vector<Tempest::Vec3> furledVertices={{0,100,-40},{1000,100,-40},{0,200,-40},{1000,200,-40},
                                                {0,100,40},{1000,100,40},{0,200,40},{1000,200,40}};
  const std::vector<uint32_t> furledIndices={0,1,2,1,3,2,4,6,5,5,6,7,0,2,4,2,6,4,1,5,3,3,5,7};
  auto yardMatrix=Tempest::Matrix4x4::mkIdentity();
  yardMatrix.translate(500,250,0);
  auto yard=collision.addCollisionBody(new JPH::BoxShape(JPH::Vec3(6,.05f,.35f)),yardMatrix,.7f,DynamicWorld::C_Landscape);
  yard->setLabel("world-yard");
  auto furled=cloth.add(furledVertices,furledIndices,{2,3,6,7},Tempest::Matrix4x4::mkIdentity(),.85f,"furled-sail-contract",13);
  cloth.setObserver({500,150,0});
  cloth.setWind({0,0,4});
  tick(collision,60);
  require(cloth.snapshots().front().exposed,"a furled sail outside portal rooms receives wind under its world-mesh yard");
  auto furledRoofMatrix=Tempest::Matrix4x4::mkIdentity();
  furledRoofMatrix.translate(500,500,0);
  auto furledRoof=collision.addCollisionBody(new JPH::BoxShape(JPH::Vec3(8,.1f,4)),furledRoofMatrix,.7f,DynamicWorld::C_Landscape);
  furledRoof->setLabel("furled-roof");
  tick(collision,60);
  require(cloth.snapshots().front().exposed,"ordinary landscape above an exterior sail is not an indoor ceiling");
  furled.reset();
  furledRoof.reset();
  yard.reset();
  cloth.setExposureTest({});
  cloth.setObserver({60,100,0});
  auto wallMatrix=Tempest::Matrix4x4::mkIdentity();
  wallMatrix.translate(60,100,30);
  auto wall=collision.addCollisionBody(new JPH::BoxShape(JPH::Vec3(5,5,.05f)),wallMatrix,.7f,DynamicWorld::C_Object);
  wall->setLabel("contract-wall");
  auto blocked=cloth.add(flag.vertices,flag.indices,flag.pins,Tempest::Matrix4x4::mkIdentity(),.85f,"wall-flag",10);
  cloth.setWind({0,0,8});
  tick(collision,600);
  const auto wallCloth=cloth.snapshots().front();
  float maxWallZ=0;
  for(const auto& position:wallCloth.worldPositionsCm) maxWallZ=std::max(maxWallZ,position.z);
  require(maxWallZ>20.f,"wind carries cloth into a nearby native obstacle");
  require(maxWallZ>35.f,"general world contacts are disabled to keep dense cloth groups within the physics budget");
  require(wallCloth.contactVertices==0 && wallCloth.contactBodies.empty(),"world geometry produces no native soft-body contacts");
  require(wallCloth.maxAnchorErrorCm<=.5f && wallCloth.recoveryCount==0,"fixed pins bound cloth without world contacts or recovery");
  blocked.reset();
  wall.reset();
  auto character=collision.addCollisionBody(new JPH::BoxShape(JPH::Vec3(5,5,.05f)),wallMatrix,.7f,DynamicWorld::C_ActiveBody);
  auto passThrough=cloth.add(flag.vertices,flag.indices,flag.pins,Tempest::Matrix4x4::mkIdentity(),.85f,"character-flag",11);
  tick(collision,600);
  const auto characterCloth=cloth.snapshots().front();
  float maxCharacterZ=0;
  for(const auto& position:characterCloth.worldPositionsCm) maxCharacterZ=std::max(maxCharacterZ,position.z);
  require(maxCharacterZ>35.f,"cloth passes through the same obstacle on the character layer");
  require(characterCloth.contactVertices==0 && characterCloth.contactBodies.empty(),"ignored characters produce no native soft-body contacts");
  require(characterCloth.recoveryCount==0,"ignored character contacts never destabilize cloth");
  passThrough.reset();
  character.reset();
  LongFlag longFlag;
  auto longInstance=cloth.add(longFlag.vertices,longFlag.indices,longFlag.pins,Tempest::Matrix4x4::mkIdentity(),.82f,"long-side-pinned-flag",14);
  cloth.setObserver({350,350,0});
  cloth.setWind({0,0,5});
  tick(collision,180);
  const auto longGust=cloth.snapshots().front();
  cloth.setWind({0,0,0});
  tick(collision,600);
  const auto longCalm=cloth.snapshots().front();
  require(longCalm.maxSpeedCmPerSec<10.f,"a seven-metre side-pinned flag settles below ten centimetres per second after ten seconds calm");
  require(longGust.maxSpeedEverCmPerSec<=Cloth::maxVelocityMS*100.f+1.f,"long flags stay below the same native velocity cap during gusts");
  require(longCalm.maxAnchorErrorCm<=.5f && longCalm.recoveryCount==0,"a seven-metre side-pinned flag stays attached and finite after gusts and ten seconds calm");
  longInstance.reset();
  const LongFlag coarseFlag(9);
  for(float damping:{.5f,1.5f}) {
    coarseFlagCalm(coarseFlag,damping,false);
    coarseFlagCalm(coarseFlag,damping,true);
    }
  const auto oneCollisionStep=ClothContractDriver::run(longFlag.vertices,longFlag.indices,longFlag.pins,1);
  float collisionStepDelta=0;
  for(uint32_t collisionSteps:{2u,4u}) {
    const auto subdivided=ClothContractDriver::run(longFlag.vertices,longFlag.indices,longFlag.pins,collisionSteps);
    for(size_t phase=0;phase<subdivided.size();++phase) {
      collisionStepDelta=std::max(collisionStepDelta,maxDisplacement(oneCollisionStep[phase],subdivided[phase]));
      require(subdivided[phase].maxAnchorErrorCm<=.5f && subdivided[phase].recoveryCount==0,
              "additional native collision steps preserve cloth attachments and stability");
      }
    }
  require(collisionStepDelta<.01f,"unrelated native collision subdivision leaves gust and calm cloth poses unchanged");
  auto supported=cloth.add(flag.vertices,flag.indices,flag.pins,Tempest::Matrix4x4::mkIdentity(),.85f,"support-capsule-flag",15);
  supported->setAttachmentCapsule({60,-50,35},{60,250,35},5.f);
  cloth.setObserver({60,100,0});
  cloth.setWind({0,0,8});
  tick(collision,600);
  const auto supportCloth=cloth.snapshots().front();
  float closestSupport=10000.f;
  for(const auto& point:supportCloth.worldPositionsCm) {
    const float dx=point.x-60.f,dz=point.z-35.f;
    closestSupport=std::min(closestSupport,std::sqrt(dx*dx+dz*dz));
    }
  require(closestSupport>=5.49f,"the cloth's own attachment capsule excludes particles without broadphase contacts");
  require(supportCloth.contactVertices==0 && supportCloth.maxAnchorErrorCm<=.5f && supportCloth.recoveryCount==0,
          "the attachment guard preserves fixed vertices without native contacts or recovery");
  supported.reset();
  auto fastSupport=cloth.add({{0,200,0},{120,200,0},{60,0,0}},{0,1,2},{0,1},Tempest::Matrix4x4::mkIdentity(),.85f,"support-fast-hit",16);
  auto fastReference=cloth.add({{0,200,0},{120,200,0},{60,0,0}},{0,1,2},{0,1},Tempest::Matrix4x4::mkIdentity(),.85f,"support-fast-reference",17);
  fastSupport->setAttachmentCapsule({60,-50,8},{60,250,8},5.f);
  cloth.setWind({0,0,0});
  tick(collision,40); // Complete the native/fallback blend before comparing motion.
  for(const auto& snapshot:cloth.snapshots())
    require(snapshot.simulationWeight==1.f,"fast-attachment comparison begins after the transition to simulation is complete");
  require(cloth.impulse({60,0,0},50,{0,0,15})==2,"a strong local spell impulse reaches both guarded and reference particles");
  const auto stepsBeforeImpulse=cloth.snapshots().front().simulationSteps;
  for(size_t i=0;i<34 && cloth.snapshots().front().simulationSteps==stepsBeforeImpulse;++i) collision.tick(1);
  require(cloth.snapshots().front().simulationSteps==stepsBeforeImpulse+1,"a local impulse reaches the next native cloth step");
  collision.tick(20); // Show most of the completed fixed step through interpolation.
  const auto fastSnapshots=cloth.snapshots();
  const auto& fastSupportCloth=fastSnapshots.front();
  require(fastSnapshots.back().worldPositionsCm[2].z>13.5f,"without an attachment guard the same fast particle crosses beyond the pole's far side");
  require(fastSupportCloth.worldPositionsCm[2].z<=2.51f,"a fast particle cannot tunnel through its narrow attachment capsule in one physics step");
  require(fastSupportCloth.maxAnchorErrorCm<=.5f && fastSupportCloth.recoveryCount==0,"the swept attachment guard keeps both fixed vertices unchanged");
  std::printf("cloth contract passed: calm_10s_max_vertex_cm_s=%.6g max_vertex_cm_s=%.6g anchor_drift_cm=%.6g boundary_delta_cm=%.6g roof_calm_lateral_cm_s=%.6g roof_wind_lateral_cm_s=%.6g wall_vertex_max_z_cm=%.6g character_vertex_max_z_cm=%.6g long_flag_gust_max_cm_s=%.6g long_flag_calm_10s_cm_s=%.6g collision_steps_pose_delta_cm=%.6g support_min_radius_cm=%.6g support_fast_vertex_z_cm=%.6g cadence_pose_delta_cm=%.6g recovery=0\n",
              double(calm.maxSpeedCmPerSec),double(windy.maxSpeedEverCmPerSec),double(windy.maxAnchorErrorCm),
              double(boundaryDelta),double(calmLateral),double(shelteredLateral),double(maxWallZ),double(maxCharacterZ),
              double(longGust.maxSpeedEverCmPerSec),double(longCalm.maxSpeedCmPerSec),double(collisionStepDelta),double(closestSupport),double(fastSupportCloth.worldPositionsCm[2].z),double(cadenceDelta));
  return EXIT_SUCCESS;
  }

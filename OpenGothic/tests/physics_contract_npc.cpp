#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "physics_contract_common.h"
#include "gothic.h"
#include "asset/fromzenkit.h"
#include "physics/collisionworld.h"
#include "physics/physicvbo.h"
#include "physics/ragdoll.h"
#include "graphics/mesh/protomesh.h"
#include "graphics/mesh/skeleton.h"
#include "graphics/mesh/submesh/packedmesh.h"

#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <zenkit/ModelHierarchy.hh>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <memory>
#include <random>
#include <vector>

namespace PhysicsContract {
  void testNpcContactDistance() {
    auto contact = [](float diagonal, bool outside, bool rotated) {
      CollisionWorld world;
      const std::vector<JPH::Vec3> vertices = outside ? std::vector<JPH::Vec3>{
        {.104f,-.3f,-.2f},{.104f,.3f,-.2f},{.104f,0,.2f},
        {100,100,100},{101,100,100},{100,101,100},
        } : std::vector<JPH::Vec3>{
        {diagonal,-.3f,diagonal},{diagonal,.3f,diagonal},{diagonal+.2f,0,diagonal-.2f},
        {100,100,100},{101,100,100},{100,101,100},
        };
      PhysicVbo mesh(&vertices);
      const std::vector<uint32_t> indices = {0,1,2,3,4,5};
      mesh.addIndex(indices,0,indices.size(),asset::MaterialGroup::STONE,"contact-test");
      auto matrix = at(0,0,0);
      if(rotated)
        matrix.rotateOY(45);
      auto body = world.addCollisionBody(mesh.shape(),matrix,.75f,DynamicWorld::C_Landscape,false,false,&mesh);
      JPH::CapsuleShape capsule(.1f,.1f);
      Tempest::Vec3 normal;
      Interactive* vob = nullptr;
      const bool result = world.hasCollision(&capsule,{0,0,0},normal,vob);
      if(result)
        require(normal.x<-.5f && normal.z<-.5f,"separated diagonal contact must face away from its triangle");
      return result;
      };
    // The previous contact callback reported this 40 mm gap even though the
    // relative contact threshold is only 4.9 mm. Its final GJK distance bound
    // included the capsule radius. Movement consumes these separated contacts.
    require(contact(.099f,false,false),"NPC query must preserve separated contacts inside the original triangle AABB");
    // Expanding the distance query also reaches this 4 mm gap, but the original
    // triangle candidate box excluded it. The distant anchor admits broad phase.
    require(!contact(0,true,false),"NPC proximity must retain the unexpanded triangle candidate box");
    require(!contact(0,true,true),"rotated NPC mesh candidates must use the original mesh-local box");

    CollisionWorld world;
    const std::vector<JPH::Vec3> vertices = {
      {.0207614303f,.400143862f,-.305011988f},
      {.250788718f,.276874304f,.318776399f},
      {.225168914f,-.396727622f,-.0763103366f},
      {100,100,100},{101,100,100},{100,101,100},
      };
    PhysicVbo mesh(&vertices);
    const std::vector<uint32_t> indices = {0,1,2,3,4,5};
    mesh.addIndex(indices,0,indices.size(),asset::MaterialGroup::STONE,"bounded-contact-test");
    auto body = world.addCollisionBody(mesh.shape(),at(0,0,0),.75f,DynamicWorld::C_Landscape,false,false,&mesh);
    JPH::CapsuleShape capsule(.3f,.08f);
    Tempest::Vec3 normal;
    Interactive* vob = nullptr;
    // Its true surface gap is 50.8 mm, inside the 87.93 mm distance envelope,
    // and its triangle AABB overlaps. The old bounded GJK stopped with a 98.7 mm
    // simplex approximation and excluded it; accepting the exact gap blocks early.
    require(!world.hasCollision(&capsule,{0,0,0},normal,vob),
            "NPC contact eligibility must preserve the bounded query's previous simplex");
    }

  void testNpcFrozenBodyReplacement() {
    zenkit::ModelHierarchy hierarchy{};
    hierarchy.collision_bbox={{-25,0,-25},{25,180,25}};
    Skeleton shape(asset::fromZenKit(hierarchy),nullptr,"FROZEN_NPC_REPLACEMENT_CONTRACT");
    DynamicWorld world(nullptr,movableFloor());
    auto first=world.addGhostObj(&shape);
    auto middle=world.addGhostObj(&shape);
    auto last=world.addGhostObj(&shape);
    first.setPosition({0,0,0});
    middle.setPosition({200,0,0});
    last.setPosition({400,0,0});
    int identity=0;
    auto* npc=reinterpret_cast<Npc*>(&identity);
    last.setUserPointer(npc);
    world.tick(0);
    world.tick(0); // freeze all three bodies before replacing the first

    // Npc::setVisual, including MdlVisual::load, replaces its existing ghost
    // this way. Later NPC movement must use the shifted frozen-list index.
    first=world.addGhostObj(&shape);
    first.setPosition({-600,0,0});
    last.setPosition({1000,0,0});
    last.moveVirtual({},0,50.f,float(M_PI_4),80.f);
    int hits=0;
    world.rayArrow({900,90,0},{1100,90,0},[&](Npc& hit,const Tempest::Vec3&,const Tempest::Vec3&,float&) {
      if(&hit==npc)
        ++hits;
      return false;
      });
    std::printf("Frozen NPC replacement: moved body records %d (expected 1)\n",hits);
    require(hits==1,"replacing a frozen NPC body must not duplicate another moving NPC record");

    // A duplicate would survive this deletion and become a dangling rider in
    // DynamicWorld::tick when that body had a CharacterVirtual controller.
    last=DynamicWorld::NpcItem();
    const auto cleared=world.rayNpc({900,90,0},{1100,90,0},nullptr);
    require(cleared.npcHit==nullptr,"removing a moved NPC must remove its only broad-phase record");
    world.tick(0);
    }

  void testNpcScalarWitness() {
    CollisionWorld world;
    // This source face stopped the player one frame early in ik-sweep. The
    // scalar witness keeps the tiny vertical direction that selects the next
    // capsule support endpoint; the selected source geometry stays intact.
    const std::vector<JPH::Vec3> vertices = {
      {368.17807f,44.3249741f,-322.601501f},
      {366.899994f,45.7681885f,-329.008514f},
      {368.358978f,43.3533669f,-322.625275f},
      };
    PhysicVbo mesh(&vertices);
    mesh.addIndex({0,2,1},0,3,asset::MaterialGroup::STONE,"npc-witness-face");
    auto body = world.addCollisionBody(mesh.shape(),at(0,0,0),.75f,
                                       DynamicWorld::C_Landscape,false,false,&mesh);
    JPH::CapsuleShape capsule(.231234491f,.0817193016f);
    Tempest::Vec3 normal;
    Interactive* vob = nullptr;
    require(!world.hasCollision(&capsule,{36718.7188f,4488.04932f,-32690.3535f},normal,vob),
            "NPC sweep frame 19 must retain the unblocked source-face pose");
    require(world.hasCollision(&capsule,{36720.9766f,4488.3374f,-32688.2539f},normal,vob),
            "NPC sweep frame 20 must block at the next source-face pose");
    // Frozen original scalar callback direction from this accepted pose.
    requireNear(normal.x,-.976384342f,.000002f,"NPC separated contact must preserve the original conservative X direction");
    requireNear(normal.y,-.126087219f,.000002f,"NPC separated contact must preserve the original conservative Y direction");
    requireNear(normal.z,.175430045f,.000002f,"NPC separated contact must preserve the original conservative Z direction");

    // These neighboring poses exercise the tetrahedron's BDC/ADB face weights.
    // Changing interpolation order along with face evaluation regresses the second.
    require(!world.hasCollision(&capsule,{36717.918f,4487.64941f,-32690.9551f},normal,vob),
            "tetrahedron face weights must reject the original boundary pose");
    require(world.hasCollision(&capsule,{36717.7148f,4488.04932f,-32691.7539f},normal,vob),
            "tetrahedron face weights must preserve the neighboring accepted pose");
    }

  void testNpcSeparatedContactResponse() {
    CollisionWorld world;
    // Frozen tick 80027 of ik-sweep: the same rejected downward movement touched
    // an edge and an interior face. A tiny normal change redirected the fall,
    // then changed the next slope heading and the later collision frame.
    const std::vector<JPH::Vec3> vertices = {
      {86.245079f,5.06495762f,118.298805f},
      {80.3646851f,5.83244085f,123.314186f},
      {80.3646851f,7.01338053f,123.314186f},
      {80.3646851f,7.01338053f,123.314186f},
      {86.9217529f,5.54688644f,119.338539f},
      {86.245079f,5.06495762f,118.298805f},
      };
    PhysicVbo mesh(&vertices);
    mesh.addIndex({0,2,1,3,5,4},0,6,asset::MaterialGroup::STONE,"npc-fall-contact");
    auto body = world.addCollisionBody(mesh.shape(),at(0,0,0),.75f,
                                       DynamicWorld::C_Landscape,false,false,&mesh);
    JPH::CapsuleShape capsule(.231234491f,.0817193016f);
    Tempest::Vec3 normal;
    Interactive* vob = nullptr;
    require(world.hasCollision(&capsule,{8081.23877f,719.952087f,12290.1709f},normal,vob),
            "NPC fall contact must block the frozen downward sample");
    requireNear(normal.x,.00157767383f,.0000002f,"NPC fall contact must preserve original edge/face normal X");
    requireNear(normal.y,.932768404f,.0000002f,"NPC fall contact must preserve original edge/face normal Y");
    requireNear(normal.z,-.360472828f,.0000002f,"NPC fall contact must preserve original edge/face normal Z");

    // Recorded gravity-controller response to this public collision normal.
    // Test the branch's slope redirection without requiring NPC/world assets.
    const float horizontal = std::sqrt(normal.x*normal.x+normal.z*normal.z);
    const float complement = std::sqrt(1.f-horizontal*horizontal);
    const auto redirected = Tempest::Vec3(complement*normal.x/horizontal,
                                          horizontal*normal.y/complement,
                                          complement*normal.z/horizontal)*.35966f;
    requireNear(redirected.x,.00146827195f,.0000002f,"NPC fall response must preserve original horizontal velocity");
    requireNear(redirected.y,.129648894f,.0000002f,"NPC fall response must preserve original vertical velocity");
    requireNear(redirected.z,-.335476279f,.0000002f,"NPC fall response must preserve original forward velocity");
    }

  void testCharacterVirtualSkippedMove() {
    Resources resources;
    ProtoMesh wall(PackedMesh(movableCollisionBox({20,400,600}),PackedMesh::PK_Visual),"VIRTUAL_SKIP_WALL.MRM");
    zenkit::ModelHierarchy hierarchy{};
    hierarchy.collision_bbox = {{-25,0,-25},{25,180,25}};
    Skeleton shape(asset::fromZenKit(hierarchy),nullptr,"VIRTUAL_SKIP_CONTRACT");
    for(bool obstructed:{false,true}) {
      DynamicWorld world(nullptr,movableFloor());
      auto structure=obstructed ? world.addStaticObj(wall.attach[0].shape.get(),at(50,200,0),"VIRTUAL_SKIP_WALL.MRM") :
                                 DynamicWorld::Item();
      auto walker=world.addGhostObj(&shape);
      const Tempest::Vec3 initial(14,0,0);
      for(const Tempest::Vec3 request:{Tempest::Vec3(.11f,0,0),Tempest::Vec3(1.9f,0,1.9f)}) {
        walker.setPosition(initial);
        DynamicWorld::CollisionTest collision;
        require(walker.tryMove(initial+request,collision)==DynamicWorld::MC_Skip,
                "legacy skip must remain component-wise even when the diagonal length exceeds two centimetres");
        const auto moved=walker.moveVirtual(request,17,50,.785398163f,80);
        require(!moved.blocked,"a legacy-skipped animation sway must not report virtual movement failure, with or without a wall");
        }
      walker.setPosition(initial);
      DynamicWorld::CollisionTest collision;
      require(walker.tryMove(initial+Tempest::Vec3(2,0,0),collision)!=DynamicWorld::MC_Skip,
              "a component exactly two centimetres must retain the legacy non-skip boundary");
      walker.setPosition(initial);
      const auto step=walker.moveVirtual({5,0,0},17,50,.785398163f,80);
      require(step.blocked==obstructed,"a real walking step must report a blocking wall while remaining free on an unobstructed floor");
      if(obstructed)
        require(!structure.isEmpty() && step.collision.landCol,"a blocked virtual walking step must retain its world collision notification");
      else
        require(step.position.x>initial.x+4.f,"suppressing animation-sway failures must preserve native walking displacement");
      }
    }

  void testCharacterVirtualInteractiveStep() {
    constexpr float slope=.785398163f;
    for(bool movable:{false,true}) for(bool interactive:{false,true}) {
      CollisionWorld world;
      auto floor=world.addCollisionBody(new JPH::BoxShape(JPH::Vec3(20,.5f,20)),at(0,-50,0),.7f,
                                        DynamicWorld::C_Landscape);
      // A chair seat and an architectural tread can have the same geometry.
      // The interactive classification determines whether walking may step up.
      auto seat=world.addCollisionBody(new JPH::BoxShape(JPH::Vec3(.5f,.2f,1)),at(100,20,0),.7f,
                                       DynamicWorld::C_Object,movable);
      if(interactive)
        seat->setPayload(nullptr,DynamicWorld::P_Interactive);
      auto walker=world.createVirtualCharacter(25,180,{0,0,0});
      float highest=0.f;
      CollisionWorld::CharacterMoveResult state;
      for(int i=0;i<60;++i) {
        state=walker->move({5,0,0},17,50,slope,80,nullptr);
        highest=std::max(highest,state.position.y);
        }
      std::printf("CharacterVirtual 40 cm edge: movable %d, interactive %d, max feet %.4f cm, final x %.3f cm\n",
                  movable,interactive,double(highest),double(state.position.x));
      if(interactive) {
        require(highest<1.f && state.position.x<30.f,
                "automatic world stairs must not step onto or pass through interactive furniture");
        walker->setPosition({100,40,0});
        state=walker->move({},17,50,slope,80,nullptr);
        require(state.grounded,"interactive furniture must retain support for feet explicitly placed on its top");
        requireNear(state.position.y,40.f,1.f,"disabling automatic furniture stepping must not drop existing top support");
        } else {
        require(highest>39.f && state.position.x>200.f,
                "ordinary architectural C_Object treads must retain automatic world stair walking");
        }
      }
    }

  void testCharacterVirtualMovingUtensil() {
    float maximumJump=0.f;
    for(bool kicked:{false,true}) {
      CollisionWorld world;
      auto floor=world.addCollisionBody(new JPH::BoxShape(JPH::Vec3(20,.5f,20)),at(0,-50,0),.7f,
                                        DynamicWorld::C_Landscape);
      auto matrix=at(150,25,0);
      if(!kicked)
        matrix.rotateOZ(45.f);
      // Real dropped items have a centred proxy padded to 15 cm per half-axis.
      // Its mass is still calculated from the smaller visual, like ITFO_BEER.
      auto mug=world.addDynamicBody(new JPH::BoxShape(JPH::Vec3(.15f,.15f,.15f)),matrix,.5f,5.80991459f,
                                     DynamicWorld::C_Item,false);
      for(int i=0;i<90;++i)
        world.tick(17);
      auto walker=world.createVirtualCharacter(25,180,{0,0,0});
      float highest=0.f,jump=0.f,last=0.f;
      CollisionWorld::CharacterMoveResult state;
      for(int frame=0;frame<65;++frame) {
        if(kicked && frame==8) {
          const auto point=mug->position()+Tempest::Vec3(0,10,10);
          require(mug->addImpulseAtPoint({-5,0,0},point),
                  "moving utensil fixture must apply its off-centre native impulse");
          }
        state=walker->move({11,0,0},33,50,.785398163f,80,nullptr);
        highest=std::max(highest,state.position.y);
        jump=std::max(jump,state.position.y-last);
        last=state.position.y;
        world.tick(33);
        }
      maximumJump=std::max(maximumJump,std::max(highest,jump));
      std::printf("CharacterVirtual moving 5.81 kg utensil: kicked %d, max feet %.4f cm, max jump %.4f cm, final x %.3f cm\n",
                  kicked,double(highest),double(jump),double(state.position.x));
      require(mug->touchedByNpc() && mug->position().x>200.f,
              "a rotating loose utensil must retain its native character contact and walking push");
      require(state.position.x>600.f,"a moving loose utensil must not prevent ordinary walking progress");
      }
    require(maximumJump<1.f,
            "a moving or rotating light item's side velocity must not lift feet through the floor contact solver");
    }

  void testCharacterVirtualIdleSlopes() {
    constexpr float maxSlope=.785398163f;
    const auto slopeNormal=[](float degrees) {
      const float angle=degrees*.01745329252f;
      const auto heading=JPH::Vec3(.120f,0,-.071f).Normalized();
      return heading*std::sin(angle)+JPH::Vec3::sAxisY()*std::cos(angle);
      };
    // The first normal is the reported farm-path support; the others approach
    // the walkable limit and cross it. Exercise both human capsule sizes.
    const std::array<JPH::Vec3,4> normals={JPH::Vec3(.120f,.990f,-.071f).Normalized(),
                                          slopeNormal(25),slopeNormal(44),slopeNormal(60)};
    for(float radius:{25.f,20.f}) for(const auto normal:normals) {
      CollisionWorld world;
      const auto vertex=[&](float x,float z) {return JPH::Vec3(x,-(normal.GetX()*x+normal.GetZ()*z)/normal.GetY(),z);};
      JPH::TriangleList triangles;
      triangles.emplace_back(vertex(-20,-20),vertex(-20,20),vertex(20,20));
      triangles.emplace_back(vertex(-20,-20),vertex(20,20),vertex(20,-20));
      const auto mesh=JPH::MeshShapeSettings(triangles).Create();
      require(!mesh.HasError(),"idle-slope fixture must construct its landscape plane");
      auto floor=world.addCollisionBody(mesh.Get(),at(0,0,0),.7f,DynamicWorld::C_Landscape);
      auto walker=world.createVirtualCharacter(radius,180,{0,radius*(1.f/normal.GetY()-1.f),0});
      auto reference=world.createVirtualCharacter(radius,180,{0,radius*(1.f/normal.GetY()-1.f),0});
      const Tempest::Vec3 initialFeet(0,radius*(1.f/normal.GetY()-1.f),0);
      for(bool teleport:{false,true}) {
        if(teleport) {
          walker->setPosition(initialFeet);
          reference->setPosition(initialFeet);
          walker->move({},0,50,maxSlope,80,nullptr);
          }
        const auto idleContact=walker->move({},33,50,maxSlope,80,nullptr);
        // An infinitesimal vertical request uses the native gravity solve.
        // Acquiring an idle contact must be continuous with that solve,
        // including after a teleport followed by a zero-dt contact refresh.
        const auto nativeContact=reference->move({0,.0001f,0},33,50,maxSlope,80,nullptr);
        requireNear((idleContact.position-nativeContact.position).length(),0,.001f,
                    "initial idle contact recovery must preserve the native contact pose used for stairs");
        }
      CollisionWorld::CharacterMoveResult state;
      for(int frame=0;frame<30;++frame)
        state=walker->move({},17,50,maxSlope,80,nullptr);
      const auto initial=state.position;
      const bool walkable=normal.GetY()>std::cos(maxSlope);
      require(walkable ? state.grounded : state.steep,"idle-slope fixture must identify walkable and sliding support");
      for(int frame=0;frame<180;++frame)
        state=walker->move({},frame%3==0 ? 16 : 17,50,maxSlope,80,nullptr);
      const float drift=(state.position-initial).length();
      std::printf("CharacterVirtual idle slope: radius %.0f cm, incline %.3f degrees, 3 s drift %.5f cm, grounded %d, steep %d\n",
                  double(radius),double(std::acos(normal.GetY())*57.2957795f),double(drift),state.grounded,state.steep);
      if(walkable) {
        require(state.grounded && drift<.5f,"idle human capsules must drift less than half a centimetre in three seconds below the slide threshold");
        const auto jumped=walker->move({0,20,0},17,50,maxSlope,80,nullptr);
        require(!jumped.grounded && jumped.position.y>state.position.y+10.f,
                "static slope friction must preserve an explicit jump away from support");
        } else {
        require(state.steep && state.position.y<initial.y-1.f,
                "idle human capsules must retain downhill sliding above the slide threshold");
        }
      }
    }

  void testCharacterVirtualSupports() {
    constexpr float slope=.785398163f;
    // Sample every rider immediately after move(), against the support pose
    // that move() actually observed. Comparing a pre-physics rider with a
    // post-physics body hides a one-frame error inside the drift tolerance.
    const auto frameDt=[](int frame) -> uint64_t { return frame%3==2 ? 16 : 17; };
    const auto carriedPoint=[](const Tempest::Matrix4x4& matrix, Tempest::Vec3 local) {
      matrix.project(local);
      return local;
      };
    for(bool gamePhase:{false,true}) for(bool rotating:{false,true}) {
      CollisionWorld world;
      auto floor=world.addCollisionBody(new JPH::BoxShape(JPH::Vec3(20,.5f,20)),at(0,-50,0),.7f,
                                        DynamicWorld::C_Landscape);
      auto platform=world.addCollisionBody(new JPH::BoxShape(JPH::Vec3(2,.1f,2)),at(0,90,0),.7f,
                                           DynamicWorld::C_Object,true);
      // Hero and a second human-sized controller exercise distinct points of
      // the same support, including a nonzero radius for the turning platform.
      std::array<std::unique_ptr<CollisionWorld::VirtualCharacter>,2> riders={
        world.createVirtualCharacter(25,180,{100,100,-60}),
        world.createVirtualCharacter(25,180,{-80,100,70})};
      std::array<CollisionWorld::CharacterMoveResult,2> state;
      std::array<Tempest::Vec3,2> initial,local;
      std::array<float,2> maximumDrift={};
      for(size_t rider=0;rider<riders.size();++rider) {
        state[rider]=riders[rider]->move({},17,50,slope,80,nullptr);
        require(state[rider].grounded,"both virtual riders must acquire the initial kinematic support");
        initial[rider]=state[rider].position;
        local[rider]=initial[rider]-platform->position();
        }
      uint64_t elapsed=0;
      const int frames=rotating ? 180 : 240;
      bool supported=true;
      for(int frame=0;frame<frames;++frame) {
        const auto dt=frameDt(frame);
        if(gamePhase)
          for(size_t rider=0;rider<riders.size();++rider)
            state[rider]=riders[rider]->move({},dt,50,slope,80,nullptr);
        elapsed+=dt;
        auto matrix=rotating ? at(0,90,0) : at(.08f*float(elapsed),90.f+.0045f*float(elapsed),0);
        if(rotating)
          matrix.rotateOY(.04f*float(elapsed)); // 120 degrees in three seconds.
        platform->setObjMatrix(matrix);
        world.tick(dt);
        for(size_t rider=0;rider<riders.size();++rider) {
          if(gamePhase) {
            const auto carried=riders[rider]->carry(dt);
            require(carried.supportMoved,"a mover updated after NPC movement must exercise the late carry sweep");
            state[rider]=carried;
            } else {
            state[rider]=riders[rider]->move({},dt,50,slope,80,nullptr);
            }
          supported&=state[rider].grounded;
          maximumDrift[rider]=std::max(maximumDrift[rider],
                                      (state[rider].position-carriedPoint(matrix,local[rider])).length());
          }
        }
      const auto matrix=platform->matrix();
      for(size_t rider=0;rider<riders.size();++rider) {
        const auto expected=carriedPoint(matrix,local[rider]);
        const auto drift=state[rider].position-expected;
        const auto displacement=state[rider].position-initial[rider];
        std::printf("CharacterVirtual %s %s support rider %zu: support travel %.6f cm, angle %.3f deg, "
                    "rider displacement (%.6f, %.6f, %.6f) cm, drift (%.6f, %.6f, %.6f) cm, "
                    "final %.6f / maximum %.6f cm, ground speed %.6f cm/s\n",
                    gamePhase ? "NPC->mover->physics->carry" : "physics->NPC",
                    rotating ? "turning kinematic" : "linear kinematic",rider,
                    double((platform->position()-Tempest::Vec3(0,90,0)).length()),
                    rotating ? .04*double(elapsed) : 0.,
                    double(displacement.x),double(displacement.y),double(displacement.z),
                    double(drift.x),double(drift.y),double(drift.z),double(drift.length()),
                    double(maximumDrift[rider]),double(state[rider].groundVelocity.length()));
        }
      require(supported,"both virtual riders must retain kinematic support throughout movement");
      if(rotating)
        require(.04f*float(elapsed)>=90.f,"the turning support contract must rotate at least ninety degrees");
      else
        require(platform->position().x>=300.f,"the linear support contract must travel at least three metres");
      for(const float drift:maximumDrift)
        requireNear(drift,0,rotating ? 2.f : 1.f,
                    rotating ? "a turning support must retain each rider's local point within two centimetres" :
                               "a three-metre kinematic translation must carry each rider within one centimetre");
      require(state[0].groundVelocity.length()>50.f,
              "kinematic support must expose its actual support-point velocity for a later jump");
      // No residual inferred velocity is allowed after the transform stops.
      for(size_t rider=0;rider<riders.size();++rider) {
        const auto stopped=riders[rider]->move({},17,50,slope,80,nullptr);
        requireNear((stopped.position-state[rider].position).length(),0,.1f,
                    "stopping a direct-matrix support must not retain its previous carried displacement");
        if(gamePhase) {
          world.tick(17);
          const auto carried=riders[rider]->carry(17);
          require(!carried.supportMoved,"a stopped mover must not schedule another carry sweep");
          requireNear((carried.position-stopped.position).length(),0,.001f,
                      "the late carry phase must not move feet after their support has stopped");
          }
        }
      platform.reset();
      if(gamePhase)
        require(!riders[0]->carry(17).supportMoved,"removing a support must invalidate its late carry sweep");
      const auto falling=riders[0]->move({},17,50,slope,80,nullptr);
      require(!falling.grounded && falling.position.y<state[0].position.y,
              "virtual feet must lose support and fall after the moving platform disappears");
      }
    for(bool gamePhase:{false,true}) for(bool variableCadence:{false,true}) {
      if(!gamePhase && variableCadence)
        continue; // Preserve the original nominal physics-before-NPC contract.
      CollisionWorld world;
      auto floor=world.addCollisionBody(new JPH::BoxShape(JPH::Vec3(20,.5f,20)),at(0,-50,0),.01f,
                                        DynamicWorld::C_Landscape);
      // A heavy, broad platform has an accepted horizontal top under the step-1
      // support policy. Low floor friction makes the impulse travel observable;
      // a nearly stationary body cannot establish a carry-distance contract.
      auto platform=world.addDynamicBody(new JPH::BoxShape(JPH::Vec3(2,.1f,2)),at(0,10,0),.01f,122.f,
                                         DynamicWorld::C_MovableVob,true);
      auto rider=world.createVirtualCharacter(25,180,{0,20,0});
      auto state=rider->move({},17,50,slope,80,nullptr);
      require(state.grounded,"virtual feet must acquire the heavy dynamic support before its impulse");
      require(platform->mass()>=10.f,"the dynamic carry contract must use an accepted support mass");
      const auto bodyStart=platform->position(),riderStart=state.position;
      const auto local=riderStart-bodyStart;
      require(platform->addImpulse({1.8f*platform->mass(),0,0}),
              "the dynamic support must receive one native linear impulse");
      float maximumDrift=0.f,maximumTravel=0.f;
      Tempest::Vec3 endpointPrediction;
      float maximumPredictionError=0.f,spikeObserved=0.f,spikePredicted=0.f;
      bool supported=true;
      for(int frame=0;frame<120;++frame) {
        constexpr std::array<uint64_t,4> cadence={25,45,16,65};
        const auto dt=variableCadence ? cadence[size_t(frame)%cadence.size()] : frameDt(frame);
        if(gamePhase)
          state=rider->move({},dt,50,slope,80,nullptr);
        // Physics advances before this character sample, just as the preceding
        // World::tick has already advanced bodies before the next NPC tick.
        const auto previous=platform->position();
        world.tick(dt);
        const auto velocityBeforeCarry=platform->velocity();
        const auto angularBeforeCarry=platform->angularVelocity();
        endpointPrediction+=velocityBeforeCarry*(float(dt)*.001f);
        maximumPredictionError=std::max(maximumPredictionError,
                                        (endpointPrediction-(platform->position()-bodyStart)).length());
        if(dt==65) {
          spikeObserved=(platform->position()-previous).length();
          spikePredicted=velocityBeforeCarry.length()*.065f;
          }
        if(gamePhase) {
          const auto carried=rider->carry(dt);
          if(carried.supportMoved)
            state=carried;
          requireNear((platform->velocity()-velocityBeforeCarry).length(),0,.0001f,
                      "a late carry sweep must not apply a second weight or contact impulse to its support");
          requireNear((platform->angularVelocity()-angularBeforeCarry).length(),0,.000001f,
                      "a late carry sweep must not apply torque to its support");
          } else {
          state=rider->move({},dt,50,slope,80,nullptr);
          }
        supported&=state.grounded;
        maximumTravel=std::max(maximumTravel,(platform->position()-bodyStart).length());
        maximumDrift=std::max(maximumDrift,(state.position-carriedPoint(platform->matrix(),local)).length());
        }
      const auto drift=state.position-carriedPoint(platform->matrix(),local);
      std::printf("CharacterVirtual %s dynamic support %s: mass %.6f kg, impulse %.6f Ns, "
                  "support travel %.6f cm, rider travel %.6f cm, drift (%.6f, %.6f, %.6f) cm, "
                  "final %.6f / maximum %.6f cm; endpoint-v*dt prediction error %.6f cm, "
                  "last 65 ms observed %.6f vs predicted %.6f cm\n",
                  gamePhase ? "NPC->physics->carry" : "physics->NPC",variableCadence ? "25/45/16/65 ms" : "17/17/16 ms",
                  double(platform->mass()),double(1.8f*platform->mass()),
                  double(maximumTravel),double((state.position-riderStart).length()),
                  double(drift.x),double(drift.y),double(drift.z),double(drift.length()),double(maximumDrift),
                  double(maximumPredictionError),double(spikeObserved),double(spikePredicted));
      require(maximumTravel>=50.f,"the impulse-driven dynamic support must travel at least fifty centimetres");
      require(supported,"the rider must remain grounded throughout the dynamic carry contract");
      requireNear(maximumDrift,0,2.f,"a heavy impulse-driven support must carry virtual feet within two centimetres");
      if(variableCadence)
        require(maximumPredictionError>2.f,
                "the variable cadence must expose endpoint velocity integration differing from native fixed-step displacement");
      }
    {
      CollisionWorld world;
      auto floor=world.addCollisionBody(new JPH::BoxShape(JPH::Vec3(20,.5f,20)),at(0,-50,0),.7f,
                                        DynamicWorld::C_Landscape);
      auto platform=world.addDynamicBody(new JPH::BoxShape(JPH::Vec3(2,.1f,2)),at(0,90,0),.7f,122.f,
                                          DynamicWorld::C_MovableVob);
      auto rider=world.createVirtualCharacter(25,180,{0,100,0});
      auto state=rider->move({},17,50,slope,80,nullptr);
      require(state.grounded,"the reversing support fixture must acquire its dynamic deck");
      const auto bodyStart=platform->position();
      auto local=state.position;
      auto inverse=platform->matrix();inverse.inverse();inverse.project(local);
      require(platform->addImpulse({0,platform->mass()*(1.8f-platform->velocity().y*.01f),0}),
              "the dynamic deck must accept an upward impulse before reversing under gravity");
      float maximumDrift=0.f, maximumRise=0.f;
      bool descended=false;
      for(int frame=0;frame<180;++frame) {
        const uint64_t dt=frame%3==2 ? 34 : 33;
        state=rider->move({},dt,50,slope,80,nullptr);
        const auto previous=platform->position();
        world.tick(dt);
        const auto carried=rider->carry(dt);
        if(carried.supportMoved)
          state=carried;
        require(state.grounded,"an idle rider must retain support while its deck reverses vertical direction");
        maximumRise=std::max(maximumRise,platform->position().y-bodyStart.y);
        descended|=platform->position().y<previous.y-.01f;
        maximumDrift=std::max(maximumDrift,(state.position-carriedPoint(platform->matrix(),local)).length());
        }
      std::printf("CharacterVirtual reversing dynamic deck: rise %.6f cm, descended %d, maximum carry error %.6f cm\n",
                  double(maximumRise),descended,double(maximumDrift));
      require(maximumRise>1.f && descended,"the dynamic carry fixture must actually rise and reverse");
      require(maximumDrift<=.1f,
              "an idle rider must not add predicted support motion before being carried by the observed transform");
      }
    {
      CollisionWorld world;
      auto platform=world.addCollisionBody(new JPH::BoxShape(JPH::Vec3(4,.1f,2)),at(0,90,0),.7f,
                                           DynamicWorld::C_Object,true);
      auto wall=world.addCollisionBody(new JPH::BoxShape(JPH::Vec3(.1f,2,2)),at(100,200,0),.7f,
                                       DynamicWorld::C_Object);
      auto rider=world.createVirtualCharacter(25,180,{0,100,0});
      auto state=rider->move({},17,50,slope,80,nullptr);
      require(state.grounded,"the blocked carry fixture must begin on its kinematic support");
      for(int frame=1;frame<=160;++frame) {
        state=rider->move({},17,50,slope,80,nullptr);
        platform->setObjMatrix(at(2.f*float(frame),90,0));
        world.tick(17);
        const auto carried=rider->carry(17);
        if(carried.supportMoved)
          state=carried;
        require(state.position.x<=65.1f,"a late carry sweep must stop at a world wall instead of pinning feet to the support");
        }
      std::printf("CharacterVirtual late carry blocked by wall: support %.6f cm, rider %.6f cm, slip %.6f cm\n",
                  double(platform->position().x),double(state.position.x),double(platform->position().x-state.position.x));
      require(state.position.x>50.f && platform->position().x-state.position.x>250.f,
              "the wall fixture must carry the rider to the obstruction while the support continues underneath");
      }
    }

  void testCharacterVirtualSlopeRest() {
    constexpr float radians=.01745329252f, maxSlope=50.f*radians;
    constexpr float radius=25.f, padding=1.f;
    for(unsigned planeKind:{0u,1u,2u}) for(float angle:{14.f,25.f,40.f,49.f,55.f}) {
      const float sine=std::sin(angle*radians), cosine=std::cos(angle*radians);
      CollisionWorld world;
      std::unique_ptr<CollisionWorld::CollisionBody> support;
      if(planeKind>0) {
        const JPH::Vec3 along(cosine,sine,0), across(0,0,1);
        const auto a=-20.f*along-20.f*across, b=-20.f*along+20.f*across;
        const auto c=20.f*along+20.f*across, d=20.f*along-20.f*across;
        JPH::TriangleList triangles;
        triangles.emplace_back(a,b,c);
        triangles.emplace_back(a,c,d);
        const auto shape=JPH::MeshShapeSettings(triangles).Create();
        require(!shape.HasError(),"slope rest fixture must construct a planar mesh");
        JPH::ShapeRefC floorShape=shape.Get();
        if(planeKind==2) {
          // The real landscape is chunked into compound mesh children. Keep
          // both hierarchy bits and the triangle ID in the support subshape.
          JPH::StaticCompoundShapeSettings inner;
          inner.AddShape(JPH::Vec3::sZero(),JPH::Quat::sIdentity(),floorShape.GetPtr());
          inner.AddShape(JPH::Vec3(100,0,0),JPH::Quat::sIdentity(),new JPH::BoxShape(JPH::Vec3(1,1,1)));
          const auto child=inner.Create();
          require(!child.HasError(),"slope fixture must construct a mesh chunk");
          JPH::StaticCompoundShapeSettings outer;
          outer.AddShape(JPH::Vec3::sZero(),JPH::Quat::sIdentity(),child.Get());
          outer.AddShape(JPH::Vec3(200,0,0),JPH::Quat::sIdentity(),new JPH::BoxShape(JPH::Vec3(1,1,1)));
          const auto compound=outer.Create();
          require(!compound.HasError(),"slope fixture must construct a chunked landscape");
          floorShape=compound.Get();
          }
        support=world.addCollisionBody(floorShape,at(0,0,0),.7f,DynamicWorld::C_Landscape);
        } else {
        // Put the rotated box's upper face through the origin.
        auto matrix=at(50.f*sine,-50.f*cosine,0);
        matrix.rotateOZ(angle);
        support=world.addCollisionBody(new JPH::BoxShape(JPH::Vec3(20,.5f,20)),matrix,.7f,
                                       DynamicWorld::C_Landscape);
        }
      // The lower capsule sphere, including native character padding, touches
      // the plane at t=0. No settling frames may hide first-second movement.
      const Tempest::Vec3 initial(0,(radius+padding)/cosine-radius-padding,0);
      auto walker=world.createVirtualCharacter(radius,180,initial);
      auto state=walker->move({},0,50,maxSlope,80,nullptr);
      requireNear((state.position-initial).length(),0.f,.001f,"zero-dt slope refresh must preserve the analytical initial pose");
      requireNear(state.groundNormal.y,cosine,.001f,"initial slope contact must use the actual plane normal");
      require(angle>50.f ? state.steep : state.grounded,"slope fixture must acquire its initial support without a warmup");
      Tempest::Vec3 atOneSecond;
      float firstSecond=0.f, later=0.f;
      // Include the game's 33 ms acquisition step: one native-gravity solve
      // alone exceeds 0.5 cm at 49 degrees even from an exact tangent pose.
      for(int frame=1;frame<=300;++frame) {
        state=walker->move({},frame%3==0 ? 34 : 33,50,maxSlope,80,nullptr);
        if(frame<=30)
          firstSecond=std::max(firstSecond,(state.position-initial).length());
        if(frame==30)
          atOneSecond=state.position;
        if(frame>30)
          later=std::max(later,(state.position-atOneSecond).length());
        }
      std::printf("CharacterVirtual %s slope %.0f deg: 0-1 s %.6f cm, 1-10 s %.6f cm, final (%.6f,%.6f,%.6f) cm\n",
                  planeKind==2?"compound mesh":planeKind==1?"mesh":"box",double(angle),double(firstSecond),double(later),
                  double(state.position.x),double(state.position.y),double(state.position.z));
      if(angle<=50.f) {
        require(firstSecond<=.5f,"an idle virtual human must move at most half a centimetre during the first second on a walkable slope");
        require(later<=.5f,"an idle virtual human must move at most half a centimetre from one to ten seconds on a walkable slope");
        require(state.grounded,"rest correction must preserve walkable slope support");
        } else {
        require(state.steep && state.position.x<initial.x-5.f && state.position.y<initial.y-5.f,
                "a non-walkable slope must retain downhill sliding instead of acquiring idle friction");
        }
      }
    }

  void testCharacterVirtualStandingSway() {
    // An armed stance (S_1HRUN, S_BOWRUN) sways the root: MoveAlgo passes a
    // small oscillating X/Z displacement while the body stands. A standing
    // character keeps static friction on a walkable slope; without the flag
    // the same sway slides it downhill, as before 2026-11-03.
    constexpr float radians=.01745329252f, angle=22.f*radians, maxSlope=50.f*radians;
    constexpr float radius=25.f, padding=1.f;
    const float sine=std::sin(angle), cosine=std::cos(angle);
    float drift[2]={};
    for(bool standing:{true,false}) {
      CollisionWorld world;
      auto matrix=at(50.f*sine,-50.f*cosine,0);
      matrix.rotateOZ(22.f);
      auto support=world.addCollisionBody(new JPH::BoxShape(JPH::Vec3(20,.5f,20)),matrix,.7f,DynamicWorld::C_Landscape);
      const Tempest::Vec3 initial(0,(radius+padding)/cosine-radius-padding,0);
      auto walker=world.createVirtualCharacter(radius,180,initial);
      auto state=walker->move({},0,50,maxSlope,80,nullptr);
      require(state.grounded,"standing sway fixture must start supported on its walkable incline");
      uint64_t elapsed=0;
      for(int frame=1;frame<=240;++frame) {
        const uint64_t dt=frame%3==0 ? 34 : 33;
        elapsed+=dt;
        const float sway=.15f*std::sin(float(elapsed)*0.001f*float(M_PI));
        state=walker->move({sway,0,sway},dt,50,maxSlope,80,nullptr,false,standing);
        }
      drift[standing ? 0 : 1]=std::hypot(state.position.x-initial.x,state.position.z-initial.z);
      require(state.grounded,"standing sway must retain walkable slope support");
      }
    std::printf("CharacterVirtual 22 deg standing sway 8 s: standing %.4f cm, not standing %.4f cm\n",
                double(drift[0]),double(drift[1]));
    require(drift[0]<1.f,"a standing character whose stance sways its root must drift less than 1 cm in 8 s on a walkable slope");
    require(drift[1]>10.f,"the same sway without the standing flag must keep the ordinary gravity response");
    }

  void testCharacterVirtualIdleStairIntent() {
    constexpr float radians=.01745329252f, angle=14.f*radians, maxSlope=50.f*radians;
    constexpr float radius=25.f, padding=1.f;
    const float sine=std::sin(angle), cosine=std::cos(angle);
    CollisionWorld world;
    auto matrix=at(50.f*sine,-50.f*cosine,0);
    matrix.rotateOZ(14.f);
    auto incline=world.addCollisionBody(new JPH::BoxShape(JPH::Vec3(20,.5f,20)),matrix,.7f,
                                        DynamicWorld::C_Landscape);
    // Touch an architectural riser above the supporting volume. The incline
    // remains the real ground, while the riser could trigger automatic stairs
    // if the uphill component of inward gravity were mistaken for user input.
    auto riser=world.addCollisionBody(new JPH::BoxShape(JPH::Vec3(.5f,.2f,2)),
                                      at(radius+padding+50.f,20.f,0),.7f,DynamicWorld::C_Object);
    riser->setLabel("idle-slope-riser");
    const Tempest::Vec3 initial(0,(radius+padding)/cosine-radius-padding,0);
    auto walker=world.createVirtualCharacter(radius,180,initial);
    walker->setDiagnosticsEnabled(true);
    auto state=walker->move({},0,50,maxSlope,80,nullptr);
    require(state.grounded,"idle stair intent fixture must start supported on its incline");
    requireNear(state.groundNormal.y,cosine,.001f,"the incline, rather than the touching riser, must remain the initial ground");
    CharacterVirtualDiagnostics diagnostic;
    require(walker->diagnostics(diagnostic),"idle stair intent fixture must retain its initial contacts");
    const bool touchesRiser=std::any_of(diagnostic.afterStairs.contacts.begin(),diagnostic.afterStairs.contacts.end(),
                                       [](const auto& contact) {
      return contact.body.label=="idle-slope-riser" && contact.hadCollision && !contact.discarded &&
             !contact.withinSupportingVolume && !contact.walkableSlope;
      });
    require(touchesRiser,"the idle incline fixture must touch a steep riser outside its supporting volume");
    float maximumDrift=0.f, maximumHorizontalDrift=0.f;
    for(int frame=0;frame<1000;++frame) {
      state=walker->move({},10,50,maxSlope,80,nullptr);
      require(walker->diagnostics(diagnostic),"idle stair intent must retain movement diagnostics");
      require(!diagnostic.stairAttempted,"inward gravity must never request an automatic stair walk without horizontal movement input");
      const auto displacement=state.position-initial;
      maximumDrift=std::max(maximumDrift,displacement.length());
      maximumHorizontalDrift=std::max(maximumHorizontalDrift,std::hypot(displacement.x,displacement.z));
      }
    std::printf("CharacterVirtual idle 14 deg incline at riser: maximum drift %.6f cm, horizontal %.6f cm\n",
                double(maximumDrift),double(maximumHorizontalDrift));
    require(state.grounded && maximumDrift<=.5f && maximumHorizontalDrift<=.5f,
            "an idle character touching a riser on an incline must stay supported within half a centimetre for ten seconds");
    }

  void testCharacterVirtualSlopedRiser() {
    constexpr float radians=.01745329252f, maxSlope=50.f*radians;
    constexpr float radius=8.17f, height=125.f, stepHeight=60.f;
    const float run=40.f/std::tan(54.5f*radians);
    for(bool narrowTread:{false,true}) {
      CollisionWorld world;
      auto floor=world.addCollisionBody(new JPH::BoxShape(JPH::Vec3(20,.5f,20)),at(0,-50,0),.7f,
                                        DynamicWorld::C_Landscape);
      JPH::TriangleList triangles;
      const auto surface=[&](float x0,float y0,float x1,float y1) {
        const JPH::Vec3 a(x0*.01f,y0*.01f,-2), b(x0*.01f,y0*.01f,2);
        const JPH::Vec3 c(x1*.01f,y1*.01f,2), d(x1*.01f,y1*.01f,-2);
        triangles.emplace_back(a,b,c);
        triangles.emplace_back(a,c,d);
        };
      surface(0,0,run,40);
      if(narrowTread) {
        // Both the old 15 cm endpoint and the maximum lookahead can hit a
        // steep face. The short tread between them must still be discoverable.
        surface(run,40,run+18,40);
        surface(run+18,40,2.f*run+18,80);
        surface(2.f*run+18,80,220,80);
        } else {
        surface(run,40,220,40);
        }
      const auto mesh=JPH::MeshShapeSettings(triangles).Create();
      require(!mesh.HasError(),"sloped riser fixture must construct its architectural mesh");
      auto stairs=world.addCollisionBody(mesh.Get(),at(0,0,0),.7f,DynamicWorld::C_Object);
      auto walker=world.createVirtualCharacter(radius,height,{-40,0,0});
      walker->setDiagnosticsEnabled(true);
      auto state=walker->move({},0,stepHeight,maxSlope,80,nullptr);
      require(state.grounded,"sloped riser traversal must start supported on the flat approach");
      const float target=narrowTread?120.f:100.f, landing=narrowTread?80.f:40.f;
      bool sawLongProbe=false;
      float progress=state.position.x;
      uint64_t stalled=0;
      int frames=0;
      for(;frames<180 && state.position.x<target;++frames) {
        state=walker->move({4,0,0},17,stepHeight,maxSlope,80,nullptr);
        CharacterVirtualDiagnostics diagnostic;
        require(walker->diagnostics(diagnostic),"sloped riser traversal must retain native stair diagnostics");
        if(diagnostic.stairSucceeded && (diagnostic.stairProbe.testPosition-diagnostic.stairProbe.upPosition).length()>15.5f)
          sawLongProbe=true;
        if(state.position.x-progress>=1.f) {
          progress=state.position.x;
          stalled=0;
          } else {
          stalled+=17;
          }
        require(stalled<=1000,"a traversable finite sloped riser must not stall for more than one second");
        }
      std::printf("CharacterVirtual 54.5 deg riser %s tread: frames %d, extended probe %d, feet (%.5f,%.5f,%.5f) cm\n",
                  narrowTread?"narrow":"wide",frames,sawLongProbe,double(state.position.x),double(state.position.y),double(state.position.z));
      require(state.grounded && state.position.x>=target,"a finite steep riser must reach its horizontal architectural landing");
      requireNear(state.position.y,landing,2.f,"finite sloped riser traversal must finish on the expected tread");
      require(sawLongProbe,"the finite riser contract must exercise a landing beyond the fixed fifteen-centimetre stair probe");
      }
    {
      CollisionWorld world;
      const float angle=55.f*radians, sine=std::sin(angle), cosine=std::cos(angle);
      const JPH::Vec3 along(cosine,sine,0), across(0,0,1);
      const auto a=-20.f*along-20.f*across, b=-20.f*along+20.f*across;
      const auto c=20.f*along+20.f*across, d=20.f*along-20.f*across;
      JPH::TriangleList triangles;
      triangles.emplace_back(a,b,c);
      triangles.emplace_back(a,c,d);
      const auto mesh=JPH::MeshShapeSettings(triangles).Create();
      require(!mesh.HasError(),"continuous steep slope control must construct its planar mesh");
      auto plane=world.addCollisionBody(mesh.Get(),at(0,0,0),.7f,DynamicWorld::C_Landscape);
      const Tempest::Vec3 initial(0,(radius+1.f)/cosine-radius-1.f,0);
      auto walker=world.createVirtualCharacter(radius,height,initial);
      walker->setDiagnosticsEnabled(true);
      auto state=walker->move({},0,stepHeight,maxSlope,80,nullptr);
      float highest=initial.y, furthest=initial.x;
      for(int frame=0;frame<120;++frame) {
        state=walker->move({4,0,0},17,stepHeight,maxSlope,80,nullptr);
        highest=std::max(highest,state.position.y);
        furthest=std::max(furthest,state.position.x);
        CharacterVirtualDiagnostics diagnostic;
        require(walker->diagnostics(diagnostic) && !diagnostic.stairSucceeded,
                "a continuous non-walkable slope without a tread must never become an automatic stair");
        }
      std::printf("CharacterVirtual continuous 55 deg slope: uphill progress %.6f cm, height gain %.6f cm\n",
                  double(furthest-initial.x),double(highest-initial.y));
      require(furthest<=initial.x+.5f && highest<=initial.y+.5f,
              "extending stair probes must not allow requested movement to climb a continuous steep slope");
      }
    }

  void testMonsterCharacterVirtualEligibility() {
    struct Case {
      const char* name;
      bool down, flying, allowedWhenEnabled;
      };
    const Case cases[]={
      {"a living ground monster must receive an enabled CV",false,false,true},
      {"a living flying monster must always retain legacy movement",false,true,false},
      {"a down ground monster must never receive CV",true,false,false},
      {"a down flying monster must never receive CV",true,true,false}};
    Gothic::PhysicsFlags flags;
    require(!flags.monsterCharacterVirtual,"an unconfigured PhysicsFlags must start with monster CV off; [GAME] supplies the game default");
    // Exercise the actual MoveAlgo policy for every independent hero/human
    // flag combination, including an on-the-fly monster flag round trip.
    for(bool hero:{false,true}) for(bool humans:{false,true}) {
      flags.characterVirtual=hero;
      flags.npcCharacterVirtual=humans;
      for(const auto& sample:cases)
        require(flags.allowsMonsterCharacterVirtual(sample.down,sample.flying)==false,
                "a disabled monster CV must reject every state regardless of hero/human flags");
      flags.monsterCharacterVirtual=true;
      for(const auto& sample:cases)
        require(flags.allowsMonsterCharacterVirtual(sample.down,sample.flying)==sample.allowedWhenEnabled,sample.name);
      flags.monsterCharacterVirtual=false;
      for(const auto& sample:cases)
        require(flags.allowsMonsterCharacterVirtual(sample.down,sample.flying)==false,
                "toggling monster CV off must restore legacy eligibility for every state");
      }
    std::printf("Monster CharacterVirtual eligibility: unconfigured OFF; four living/down/flying cases "
                "and four independent hero/human flag combinations passed OFF/ON/OFF\n");
    }

  void testMonsterCharacterVirtualIsolatedLedges() {
    struct Ledge {
      const char* monster;
      float radius, height, stepHeight, rise;
      float bottom=0;
      };
    constexpr std::array<Ledge,6> ledges={{{"WOLF",15,83,60,20},
                                            {"WOLF",15,83,60,40},
                                            {"WOLF",15,83,60,55},
                                            {"TROLL",68,272,100,40},
                                            {"TROLL",68,272,100,80},
                                            {"WOLF",15,83,60,40,-100}}};
    struct Result {
      bool supported=false, diagnostics=true, success=false;
      uint64_t elapsed=0;
      unsigned stalls=0, maximumStall=0;
      int stairAttempts=0, steepBounces=0;
      float maximumRise=0;
      };
    std::array<std::array<Result,2>,ledges.size()> results;
    constexpr float maxSlope=50.f*.01745329252f;
    Resources resources;
    for(size_t index=0;index<ledges.size();++index) {
      const auto& ledge=ledges[index];
      auto terrain=movableFloor();
      terrain.vertices.clear();
      terrain.polygons.vertex_indices.clear();
      terrain.polygons.material_indices.clear();
      const auto surface=[&](float x0,float y0,float x1,float y1) {
        const auto first=uint32_t(terrain.vertices.size());
        terrain.vertices.insert(terrain.vertices.end(),{{x0,y0,-1000},{x0,y0,1000},
                                                        {x1,y1,1000},{x1,y1,-1000}});
        for(uint32_t vertex:{0u,2u,1u,0u,3u,2u})
          terrain.polygons.vertex_indices.push_back(first+vertex);
        terrain.polygons.material_indices.insert(terrain.polygons.material_indices.end(),{0,0});
        };
      // One vertical face with a broad flat landing, rather than a flight of
      // short risers. The approach leaves both capsules clear of the ledge.
      surface(-2000,0,100,0);
      surface(100,ledge.bottom,100,ledge.rise);
      surface(100,ledge.rise,2000,ledge.rise);
      zenkit::ModelHierarchy hierarchy{};
      hierarchy.collision_bbox={{-ledge.radius,0,-ledge.radius},
                                {ledge.radius,ledge.height,ledge.radius}};
      Skeleton skeleton(asset::fromZenKit(hierarchy),nullptr,ledge.monster);
      const float mass=Ragdoll::speciesMass(ledge.monster);
      for(int policy=0;policy<2;++policy) {
        const bool limitStairAdvance=policy!=0;
        auto& result=results[index][size_t(policy)];
        DynamicWorld world(nullptr,terrain);
        auto monster=world.addGhostObj(&skeleton);
        monster.setPosition({-200,0,0});
        monster.setVirtualDiagnosticsEnabled(true);
        auto state=monster.moveVirtual({},0,ledge.stepHeight,maxSlope,mass,nullptr,limitStairAdvance,limitStairAdvance);
        result.supported=state.grounded;
        const float target=100.f+ledge.radius+80.f;
        float progress=state.position.x;
        unsigned stalled=0;
        int frames=0;
        for(;frames<300 && state.position.x<target;++frames) {
          const uint64_t dt=frames%3==0 ? 34 : 33;
          const auto previous=state.position;
          // The same running approach for both actual controller policies:
          // 210 cm/s at the video bench's 30 Hz update cadence.
          state=monster.moveVirtual({.21f*float(dt),0,0},dt,ledge.stepHeight,maxSlope,mass,nullptr,limitStairAdvance,limitStairAdvance);
          result.steepBounces+=state.blocked && state.steepContact ? 1 : 0;
          result.elapsed+=dt;
          result.maximumRise=std::max(result.maximumRise,state.position.y-previous.y);
          if(state.position.x-progress>=1.f) {
            progress=state.position.x;
            stalled=0;
            } else {
            const unsigned next=stalled+unsigned(dt);
            if(stalled<=1000 && next>1000)
              ++result.stalls;
            stalled=next;
            result.maximumStall=std::max(result.maximumStall,stalled);
            }
          CharacterVirtualDiagnostics diagnostic;
          const bool hasDiagnostic=monster.virtualDiagnostics(diagnostic);
          result.diagnostics&=hasDiagnostic;
          if(hasDiagnostic && diagnostic.stairAttempted)
            ++result.stairAttempts;
          }
        result.success=state.grounded && state.position.x>=target && std::abs(state.position.y-ledge.rise)<=2.f;
        std::printf("Monster CharacterVirtual %s isolated %.0f cm ledge, bottom %.0f cm, policy %s: "
                    "success %d, time %llu ms, frames %d, stalls %u, max stall %u ms, "
                    "max root rise %.6f cm, stair attempts %d, steep bounces %d, feet (%.6f,%.6f,%.6f) cm\n",
                    ledge.monster,double(ledge.rise),double(ledge.bottom),limitStairAdvance ? "monster" : "old",
                    int(result.success),static_cast<unsigned long long>(result.elapsed),frames,
                    result.stalls,result.maximumStall,double(result.maximumRise),result.stairAttempts,result.steepBounces,
                    double(state.position.x),double(state.position.y),double(state.position.z));
        }
      }
    // Finish all twelve runs before asserting, so a failed policy still leaves
    // the measurements of every requested ledge available to the caller.
    for(const auto& pair:results) for(const auto& result:pair) {
      require(result.supported,"isolated monster ledges must start with native flat-ground support");
      require(result.diagnostics,"isolated monster ledges must retain actual controller diagnostics");
      }
    for(const auto& pair:results)
      require(pair[0].success,"the old controller must establish that each isolated ledge is traversable within the guild step");
    for(const auto& pair:results) {
      require(pair[1].success,"the monster stair policy must preserve traversal of an isolated ledge within the guild step");
      require(pair[1].stalls==0,"an isolated ledge within the monster guild step must not stall walking for over one second");
      require(pair[1].steepBounces==0,"a traversable isolated ledge must not notify the monster AI of a steep-wall bounce");
      }
    }

  void testMonsterCharacterVirtualShortRisers() {
    constexpr float radius=15.f, height=83.f, stepHeight=60.f;
    constexpr float maxSlope=50.f*.01745329252f;
    constexpr int stepCount=12;
    constexpr float riser=10.f;
    Resources resources;
    zenkit::ModelHierarchy hierarchy{};
    hierarchy.collision_bbox={{-radius,0,-radius},{radius,height,radius}};
    Skeleton skeleton(asset::fromZenKit(hierarchy),nullptr,"WOLF");
    const float mass=Ragdoll::speciesMass("WOLF");
    // Both stair pitches are below the fifty-degree limit. Include a narrow
    // tread so the requested stair advance is checked with closely spaced risers too.
    for(float tread:{10.f,20.f}) {
      auto terrain=movableFloor();
      terrain.vertices.clear();
      terrain.polygons.vertex_indices.clear();
      terrain.polygons.material_indices.clear();
      const auto surface=[&](float x0,float y0,float x1,float y1) {
        const auto first=uint32_t(terrain.vertices.size());
        terrain.vertices.insert(terrain.vertices.end(),{{x0,y0,-1000},{x0,y0,1000},
                                                        {x1,y1,1000},{x1,y1,-1000}});
        for(uint32_t index:{0u,2u,1u,0u,3u,2u})
          terrain.polygons.vertex_indices.push_back(first+index);
        terrain.polygons.material_indices.insert(terrain.polygons.material_indices.end(),{0,0});
        };
      surface(-1000,0,100,0);
      for(int step=0;step<stepCount;++step) {
        const float x=100.f+tread*float(step), y=riser*float(step);
        surface(x,y,x,y+riser);
        surface(x,y+riser,step+1==stepCount ? 1000.f : x+tread,y+riser);
        }
      for(bool limitStairAdvance:{false,true}) {
        DynamicWorld world(nullptr,terrain);
        auto wolf=world.addGhostObj(&skeleton);
        wolf.setPosition({50,0,0});
        wolf.setVirtualDiagnosticsEnabled(true);
        // Use the monster policy of the actual walking controller. The 60 cm
        // guild step is deliberately much higher than these repeated city-sized
        // risers; limiting a test-only stepHeight would conceal the old shortcut.
        auto state=wolf.moveVirtual({},0,stepHeight,maxSlope,mass,nullptr,limitStairAdvance,limitStairAdvance);
        require(state.grounded,"wolf short-riser contract must start supported on the flat approach");
        const float target=100.f+float(stepCount-1)*tread+80.f;
        float maximumRise=0.f, maximumDrop=0.f, progress=state.position.x;
        unsigned stalled=0, maximumStall=0, airborne=0, maximumAirborne=0;
        int stairAttempts=0, steepBounces=0, frames=0;
        for(;frames<240 && state.position.x<target;++frames) {
          const uint64_t dt=frames%3==0 ? 34 : 33;
          const auto previous=state.position;
          // 210 cm/s at the same 30 Hz cadence as the paired video bench.
          state=wolf.moveVirtual({.21f*float(dt),0,0},dt,stepHeight,maxSlope,mass,nullptr,limitStairAdvance,limitStairAdvance);
          steepBounces+=state.blocked && state.steepContact ? 1 : 0;
          maximumRise=std::max(maximumRise,state.position.y-previous.y);
          maximumDrop=std::max(maximumDrop,previous.y-state.position.y);
          if(state.position.x-progress>=1.f) {
            progress=state.position.x;
            stalled=0;
            } else {
            stalled+=unsigned(dt);
            maximumStall=std::max(maximumStall,stalled);
            }
          airborne=state.grounded ? 0 : airborne+unsigned(dt);
          maximumAirborne=std::max(maximumAirborne,airborne);
          CharacterVirtualDiagnostics diagnostic;
          require(wolf.virtualDiagnostics(diagnostic),"wolf short-riser traversal must retain native stair diagnostics");
          stairAttempts+=diagnostic.stairAttempted ? 1 : 0;
          }
        std::printf("Monster CharacterVirtual WOLF 10 cm risers / %.0f cm treads at 30 Hz, limited %d: "
                    "frames %d, stair attempts %d, steep bounces %d, max root rise %.6f cm, max drop %.6f cm, "
                    "max stall %u ms, max airborne %u ms, feet (%.6f,%.6f,%.6f) cm\n",
                    double(tread),int(limitStairAdvance),frames,stairAttempts,steepBounces,double(maximumRise),double(maximumDrop),maximumStall,maximumAirborne,
                    double(state.position.x),double(state.position.y),double(state.position.z));
        if(!limitStairAdvance)
          continue;
        require(stairAttempts>0,"the ten-centimetre wolf staircase must exercise native automatic stair walking");
        require(steepBounces==0,"a traversable short-riser staircase must not notify the monster AI of a steep-wall bounce");
        require(maximumRise<=12.01f,"a walking monster must not raise its physical root by over twelve centimetres per city-stair update");
        require(state.grounded && state.position.x>=target,"the wolf must finish the repeated ten-centimetre staircase on its upper landing");
        requireNear(state.position.y,float(stepCount)*riser,2.f,"the wolf must finish at the twelve-riser landing height");
        require(maximumStall<=1000,"the limited monster stair walk must not stall for more than one second");
        require(maximumDrop<=2.f && maximumAirborne<=100,"ascending short risers must not make the wolf fall or lose support for over one tenth of a second");
        }
      }
    }

  void testMonsterCharacterVirtualSteepBlocking() {
    constexpr float radius=15.f, height=83.f, stepHeight=60.f;
    constexpr float radians=.01745329252f, maxSlope=50.f*radians;
    Resources resources;
    zenkit::ModelHierarchy hierarchy{};
    hierarchy.collision_bbox={{-radius,0,-radius},{radius,height,radius}};
    Skeleton skeleton(asset::fromZenKit(hierarchy),nullptr,"WOLF");
    const float mass=Ragdoll::speciesMass("WOLF");
    ProtoMesh wall(PackedMesh(movableCollisionBox({20,400,2000}),PackedMesh::PK_Visual),"MONSTER_BOUNCE_WALL.MRM");
    // A 45-degree request retains half its projected advancement while sliding
    // along this face. That is deliberately above the old ten-percent gate.
    for(bool kinematic:{false,true}) for(bool bounce:{false,true}) {
      DynamicWorld world(nullptr,movableFloor());
      auto structure=kinematic ? world.addMovableObj(wall.attach[0].shape.get(),at(100,200,0),"MONSTER_BOUNCE_WALL.MRM") :
                                 world.addStaticObj(wall.attach[0].shape.get(),at(100,200,0),"MONSTER_BOUNCE_WALL.MRM");
      require(!structure.isEmpty(),"monster bounce fixture must install its static or kinematic wall");
      auto wolf=world.addGhostObj(&skeleton);
      wolf.setPosition({50,0,0});
      auto state=wolf.moveVirtual({},0,stepHeight,maxSlope,mass,nullptr,true,bounce);
      require(state.grounded,"monster wall approach must begin with native ground support");
      unsigned slidingFrames=0, blockedFrames=0;
      for(int frame=0;frame<24;++frame) {
        state=wolf.moveVirtual({5,0,5},17,stepHeight,maxSlope,mass,nullptr,true,bounce);
        if(!state.steepContact || state.advancementRatio<=.2f || state.advancementRatio>=.60f)
          continue;
        ++slidingFrames;
        blockedFrames+=state.blocked ? 1u : 0u;
        require(state.grounded,"a wall contact must preserve the wolf's flat-ground support");
        requireNear(state.steepContactNormal.x,-1.f,.001f,"a steep contact must expose the wall normal opposing the requested X motion");
        requireNear(state.steepContactNormal.y,0.f,.001f,"a steep wall normal must not use the support floor's vertical normal");
        requireNear(state.steepContactNormal.z,0.f,.001f,"the wall normal must not follow the remaining sideways displacement");
        require(state.blocked==bounce,"only the enabled monster policy must turn diagonal steep-wall sliding into a movement failure");
        if(bounce) {
          require(state.collision.landCol && state.collision.npcCol==0.f,
                  "a blocked steep wall must notify MoveAlgo through the world branch rather than its NPC branch");
          requireNear((state.collision.normal-state.steepContactNormal).length(),0.f,.001f,
                      "MoveAlgo must receive the actual steep face normal for its native bounce");
          }
        }
      std::printf("Monster CharacterVirtual 45 deg %s wall, bounce %d: sliding frames %u, blocked %u, final advancement %.6f\n",
                  kinematic ? "kinematic" : "static",int(bounce),slidingFrames,blockedFrames,double(state.advancementRatio));
      require(slidingFrames>=10,"the diagonal wall contract must exercise sustained 20-60 percent advancement instead of a head-on stop");
      }
    {
      constexpr float angle=49.f*radians;
      auto terrain=movableFloor();
      for(auto& vertex:terrain.vertices)
        vertex.y=vertex.x*std::tan(angle);
      DynamicWorld world(nullptr,terrain);
      auto wolf=world.addGhostObj(&skeleton);
      const Tempest::Vec3 initial(0,(radius+1.f)/std::cos(angle)-radius-1.f,0);
      wolf.setPosition(initial);
      auto state=wolf.moveVirtual({},0,stepHeight,maxSlope,mass,nullptr,true,true);
      require(state.grounded,"walkable monster slope control must begin with support near the guild angle limit");
      for(int frame=0;frame<30;++frame) {
        state=wolf.moveVirtual({5,0,5},17,stepHeight,maxSlope,mass,nullptr,true,true);
        require(!state.blocked && !state.steepContact,
                "the enabled monster bounce policy must never classify a walkable slope as a steep obstruction");
        }
      require(state.grounded && state.position.x>initial.x+40.f && state.position.y>initial.y+40.f,
              "the wolf must continue diagonally uphill on a walkable slope with bouncing enabled");
      }
    {
      // Keep the light box taller than the wolf's guild step. A first diagonal
      // contact therefore resembles wall sliding, but a real dynamic hull is
      // excluded from the new blocker and must still receive walking impulses.
      const Tempest::Vec3 dimensions(40,83,400);
      ProtoMesh box(PackedMesh(movableBox(dimensions),PackedMesh::PK_Visual),"MONSTER_BOUNCE_BOX.MRM");
      const float multiplier=1.f/(.4f*.83f*4.f*DynamicWorld::materialDensity(asset::MaterialGroup::WOOD));
      DynamicWorld world(nullptr,movableFloor());
      auto prop=world.addMovableVob(box,"MONSTER_BOUNCE_BOX.MRM",at(100,height*.5f,0),multiplier,true,"crate",true,47);
      require(!prop.isEmpty() && prop.mass()>.9f && prop.mass()<1.1f,
              "dynamic monster bounce control must use a real lightweight native box");
      auto wolf=world.addGhostObj(&skeleton);
      wolf.setPosition({64,0,0});
      auto state=wolf.moveVirtual({},0,stepHeight,maxSlope,mass,nullptr,true,true);
      require(state.grounded,"dynamic box contact must begin with native floor support");
      for(int frame=0;frame<60;++frame) {
        state=wolf.moveVirtual({5,0,5},17,stepHeight,maxSlope,mass,nullptr,true,true);
        require(!state.blocked && !state.steepContact,
                "a pushable dynamic box must never become a steep-wall bounce while the wolf slides along and pushes it");
        world.tick(17);
        }
      const float travel=prop.position().x-100.f;
      std::printf("Monster CharacterVirtual 45 deg dynamic %.6f kg box: pushed %.6f cm, touched %d\n",
                  double(prop.mass()),double(travel),int(prop.touchedByNpc()));
      require(prop.touchedByNpc() && travel>20.f,
              "the enabled monster bounce policy must retain actual walking impulses and movement of a light dynamic box");
      }
    {
      DynamicWorld world(nullptr,movableFloor());
      auto wolf=world.addGhostObj(&skeleton);
      auto neighbour=world.addGhostObj(&skeleton);
      wolf.setPosition({0,0,0});
      neighbour.setPosition({100,0,0});
      world.tick(0);
      world.tick(0);
      bool blockedByNpc=false;
      for(int frame=0;frame<30;++frame) {
        const auto state=wolf.moveVirtual({5,0,0},17,stepHeight,maxSlope,mass,nullptr,true,true);
        require(!state.steepContact,"another NPC must never supply the static steep-face bounce contact");
        if(state.blocked) {
          require(state.collision.npcCol>0.f && !state.collision.landCol,
                  "monster bouncing must preserve the separate native NPC collision branch");
          blockedByNpc=true;
          }
        }
      require(blockedByNpc,"the enabled monster bounce control must exercise its native neighbouring-NPC collision");
      requireNear(neighbour.position().x,100.f,0.f,"a monster's NPC collision must not move its neighbour");
      }
    }

  void testMonsterCharacterVirtual() {
    testMonsterCharacterVirtualEligibility();
    testMonsterCharacterVirtualShortRisers();
    testMonsterCharacterVirtualIsolatedLedges();
    testMonsterCharacterVirtualSteepBlocking();
    constexpr float radians=.01745329252f;
    struct Monster {
      const char* name;
      float radius, height, stepHeight, slopeDegrees;
      };
    // Radius and full height come from the wolf/troll MDH collision bbox. The
    // CV uses that full height (obj->h), just as the existing human CV path.
    // The shorter legacy query capsule removes height/2 as ghost padding;
    // capsuleHeight() reports that query shape rather than the CV's height.
    // Guild limits are the installed GOTHIC.DAT Gil_Values: wolf 60 cm,
    // troll 100 cm; both use a fifty-degree slide angle.
    const Monster monsters[]={{"WOLF",15,83,60,50},{"TROLL",68,272,100,50}};
    Resources resources;
    for(const auto& monster:monsters) {
      zenkit::ModelHierarchy hierarchy{};
      hierarchy.collision_bbox={{-monster.radius,0,-monster.radius},
                                {monster.radius,monster.height,monster.radius}};
      Skeleton skeleton(asset::fromZenKit(hierarchy),nullptr,monster.name);
      const float mass=Ragdoll::speciesMass(monster.name);
      const float maxSlope=monster.slopeDegrees*radians;
      {
        const float rise=.8f*monster.stepHeight;
        auto terrain=movableFloor();
        terrain.vertices.clear();
        terrain.polygons.vertex_indices.clear();
        terrain.polygons.material_indices.clear();
        const auto surface=[&](float x0,float y0,float x1,float y1) {
          const auto first=uint32_t(terrain.vertices.size());
          terrain.vertices.insert(terrain.vertices.end(),{{x0,y0,-1000},{x0,y0,1000},
                                                          {x1,y1,1000},{x1,y1,-1000}});
          for(uint32_t index:{0u,2u,1u,0u,3u,2u})
            terrain.polygons.vertex_indices.push_back(first+index);
          terrain.polygons.material_indices.insert(terrain.polygons.material_indices.end(),{0,0});
          };
        surface(-2000,0,100,0);
        for(int step=0;step<3;++step) {
          const float x=100.f+200.f*float(step), y=rise*float(step);
          surface(x,y,x,y+rise);
          surface(x,y+rise,step==2 ? 2000.f : x+200.f,y+rise);
          }
        DynamicWorld world(nullptr,terrain);
        auto walker=world.addGhostObj(&skeleton);
        requireNear(walker.capsuleRadius(),monster.radius,.001f,
                    "monster CV radius must use the legacy MDH landscape capsule");
        requireNear(walker.capsuleHeight(),std::max(monster.height*.5f,2.f*monster.radius),.001f,
                    "monster legacy query height must retain MDH ghost padding without changing the full-height CV");
        walker.setPosition({-100,0,0});
        auto state=walker.moveVirtual({},0,monster.stepHeight,maxSlope,mass);
        const auto traverse=[&](bool ascending) {
          float progress=state.position.x;
          unsigned stalled=0, maximumStall=0;
          int frames=0;
          for(;frames<360 && (ascending ? state.position.x<700.f : state.position.x>-100.f);++frames) {
            state=walker.moveVirtual({ascending ? 5.f : -5.f,0,0},17,monster.stepHeight,maxSlope,mass);
            if((ascending ? state.position.x-progress : progress-state.position.x)>=1.f) {
              progress=state.position.x;
              stalled=0;
              } else {
              stalled+=17;
              maximumStall=std::max(maximumStall,stalled);
              }
            }
          std::printf("Monster CharacterVirtual %s stairs %s: radius %.3f cm, height %.3f cm, "
                      "step limit %.3f cm, rise %.3f cm, frames %d, max stall %u ms, feet (%.6f,%.6f,%.6f) cm\n",
                      monster.name,ascending ? "up" : "down",double(monster.radius),double(monster.height),
                      double(monster.stepHeight),double(rise),frames,maximumStall,
                      double(state.position.x),double(state.position.y),double(state.position.z));
          require(state.grounded && (ascending ? state.position.x>=700.f : state.position.x<=-100.f),
                  "wolf and troll MDH capsules must finish architectural stairs within their step limit");
          require(maximumStall<=1000,"monster stairs must not stall requested walking for over one second");
          requireNear(state.position.y,ascending ? 3.f*rise : 0.f,2.f,
                      "monster feet must finish at the expected stair landing");
          };
        traverse(true);
        traverse(false);
        const auto saved=state.position;
        walker.resetVirtual();
        const auto restored=walker.moveVirtual({},0,monster.stepHeight,maxSlope,mass);
        requireNear((restored.position-saved).length(),0.f,.5f,
                    "recreating a monster's transient CV must preserve its persisted feet within half a centimetre");
        }
      for(float angle:{std::min(25.f,monster.slopeDegrees-1.f),monster.slopeDegrees-1.f}) {
        const float cosine=std::cos(angle*radians), tangent=std::tan(angle*radians);
        auto terrain=movableFloor();
        for(auto& vertex:terrain.vertices)
          vertex.y=vertex.x*tangent;
        DynamicWorld world(nullptr,terrain);
        auto walker=world.addGhostObj(&skeleton);
        const Tempest::Vec3 initial(0,(monster.radius+1.f)/cosine-monster.radius-1.f,0);
        walker.setPosition(initial);
        auto state=walker.moveVirtual({},0,monster.stepHeight,maxSlope,mass);
        require(state.grounded,"monster slope fixture must acquire walkable support without settling");
        requireNear(state.groundNormal.y,cosine,.001f,"monster slope fixture must retain its actual plane normal");
        for(int frame=0;frame<90;++frame)
          state=walker.moveVirtual({4,0,0},17,monster.stepHeight,maxSlope,mass);
        const auto reached=state.position;
        require(state.grounded && reached.x>initial.x+100.f && reached.y>initial.y+40.f,
                "wolf and troll MDH capsules must walk uphill below their guild slope limit");
        float maximumDrift=0;
        for(int frame=0;frame<300;++frame) {
          state=walker.moveVirtual({},frame%3==0 ? 34 : 33,monster.stepHeight,maxSlope,mass);
          maximumDrift=std::max(maximumDrift,(state.position-reached).length());
          }
        std::printf("Monster CharacterVirtual %s slope %.3f deg / limit %.3f deg: "
                    "uphill %.6f cm, height gain %.6f cm, ten-second idle drift %.6f cm\n",
                    monster.name,double(angle),double(monster.slopeDegrees),double(reached.x-initial.x),
                    double(reached.y-initial.y),double(maximumDrift));
        require(state.grounded && maximumDrift<=.5f,
                "idle wolf and troll capsules must remain supported without sliding more than half a centimetre in ten seconds");
        }
      const auto push=[&](float bodyMass,int category) {
        CollisionWorld world;
        auto floor=world.addCollisionBody(new JPH::BoxShape(JPH::Vec3(20,.5f,20)),at(0,-50,0),.7f,
                                          DynamicWorld::C_Landscape);
        const float start=monster.radius+55.f;
        auto prop=world.addDynamicBody(new JPH::BoxShape(JPH::Vec3(.2f,monster.height*.005f,2.f)),
                                       at(start,monster.height*.5f,0),.7f,bodyMass,category,true);
        auto walker=world.createVirtualCharacter(monster.radius,monster.height,{0,0,0});
        CollisionWorld::CharacterMoveResult state;
        for(int frame=0;frame<60;++frame) {
          state=walker->move({5,0,0},17,monster.stepHeight,maxSlope,mass,nullptr);
          // Sample before integration: rotating the box later cannot disguise
          // a character that has already passed through the blocking side.
          require(state.position.x<prop->position().x-monster.radius-10.f,
                  "neither a light nor heavy native dynamic hull may be crossed by a walking monster");
          world.tick(17);
          }
        const float travel=prop->position().x-start;
        std::printf("Monster CharacterVirtual %s push %.3f kg: object %.6f cm, feet %.6f cm\n",
                    monster.name,double(bodyMass),double(travel),double(state.position.x));
        require(prop->touchedByNpc(),"monster walking must attribute a real native prop contact");
        return travel;
        };
      const float light=push(1,DynamicWorld::C_Item), heavy=push(122,DynamicWorld::C_MovableVob);
      require(light>20.f && heavy<light*.5f,
              "the shared walking effort must move a light prop faster than heavy furniture for both monster sizes");
      {
        CollisionWorld world;
        // Isolate the walking impulse from gravity and friction, as in the
        // human contract. Both bodies demand effort from the same move.
        constexpr float bodyMass=20.f, seconds=.1f;
        const float x=monster.radius+20.f, y=100.f+monster.height*.5f;
        const auto dimensions=JPH::Vec3(.1f,monster.height*.005f,monster.radius*.004f);
        auto a=world.addDynamicBody(new JPH::BoxShape(dimensions),at(x,y,-monster.radius*.6f),.4f,bodyMass,
                                    DynamicWorld::C_MovableVob,true);
        auto b=world.addDynamicBody(new JPH::BoxShape(dimensions),at(x,y,monster.radius*.6f),.4f,bodyMass,
                                    DynamicWorld::C_MovableVob,true);
        auto walker=world.createVirtualCharacter(monster.radius,monster.height,{0,100,0});
        walker->move({100,0,0},100,monster.stepHeight,maxSlope,mass,nullptr);
        const float impulseA=a->velocity().length()*.01f*bodyMass;
        const float impulseB=b->velocity().length()*.01f*bodyMass;
        const float strength=std::min(mass*DynamicWorld::gravityMS,450.f);
        const float totalLimit=strength*seconds, bodyLimit=totalLimit*mass/(mass+bodyMass);
        std::printf("Monster CharacterVirtual %s shared push: mass %.3f kg, %.6f + %.6f Ns, "
                    "total limit %.6f Ns, per-body %.6f Ns\n",monster.name,double(mass),double(impulseA),
                    double(impulseB),double(totalLimit),double(bodyLimit));
        require(impulseA>1.f && impulseB>1.f,"each monster push fixture must contact two distinct native bodies");
        require(impulseA<=bodyLimit+.01f && impulseB<=bodyLimit+.01f,
                "monster contact impulses must preserve the same continuous character/body mass share as humans");
        require(impulseA+impulseB<=totalLimit+.01f,
                "all monster contacts and stair probes must share the existing walking force budget capped at 450 newtons");
        }
      }
    }

  void testCharacterVirtual() {
    testCharacterVirtualSkippedMove();
    testCharacterVirtualInteractiveStep();
    testCharacterVirtualMovingUtensil();
    testCharacterVirtualIdleSlopes();
    testCharacterVirtualSupports();
    testCharacterVirtualSlopeRest();
    testCharacterVirtualStandingSway();
    testCharacterVirtualIdleStairIntent();
    testCharacterVirtualSlopedRiser();
    testMonsterCharacterVirtual();
    constexpr float slope = .785398163f;
    const auto printState=[](const char* stage, int frames, const CollisionWorld::CharacterMoveResult& state) {
      std::fprintf(stderr,"CharacterVirtual %s: frames=%d feet=(%.3f,%.3f,%.3f) velocity=(%.3f,%.3f,%.3f) grounded=%d steep=%d normal=(%.3f,%.3f,%.3f)\n",
                   stage,frames,double(state.position.x),double(state.position.y),double(state.position.z),
                   double(state.velocity.x),double(state.velocity.y),double(state.velocity.z),state.grounded,state.steep,
                   double(state.groundNormal.x),double(state.groundNormal.y),double(state.groundNormal.z));
      };
    {
      CollisionWorld world;
      auto floor=world.addCollisionBody(new JPH::BoxShape(JPH::Vec3(20,.5f,20)),at(0,-50,0),.7f,
                                        DynamicWorld::C_Landscape);
      std::vector<std::unique_ptr<CollisionWorld::CollisionBody>> steps;
      for(int i=1;i<=3;++i)
        steps.push_back(world.addCollisionBody(new JPH::BoxShape(JPH::Vec3(.5f,.1f*float(i),2),0.f),
                                               at(100.f*float(i)+50.f,10.f*float(i),0),.7f,DynamicWorld::C_Object));
      struct Approach { const char* name; float side, heading, edgeAngle; };
      const Approach approaches[]={
        {"center",0,0,0},{"left",-10,0,0},{"right",10,0,0},
        {"heading-left",0,-15,0},{"heading-right",0,15,0},
        {"edge-walkable",0,15,44},{"edge-steep",0,-15,46}};
      for(const auto& approach:approaches) {
        constexpr float radians=.01745329252f;
        const float edgeAngle=approach.edgeAngle*radians;
        // With r=25 cm and 1 cm padding, this puts the lower sphere exactly
        // against the first tread's upper front edge. These two starts straddle
        // the 45-degree slope gate without relying on a lucky approach frame.
        const Tempest::Vec3 initial=approach.edgeAngle>0.f ?
          Tempest::Vec3(100.f-26.f*std::sin(edgeAngle),20.f+26.f*std::cos(edgeAngle)-25.f-1.f,approach.side) :
          Tempest::Vec3(0,0,approach.side);
        auto walker=world.createVirtualCharacter(25,180,initial);
        walker->setDiagnosticsEnabled(approach.edgeAngle>0.f);
        auto state=walker->move({},0,50,slope,80,nullptr);
        if(approach.edgeAngle>0.f) {
          CharacterVirtualDiagnostics diagnostic;
          require(walker->diagnostics(diagnostic),"edge approach must retain its initial native contacts");
          const bool nearThreshold=std::any_of(diagnostic.afterStairs.contacts.begin(),diagnostic.afterStairs.contacts.end(),
                                              [&](const auto& contact) {
            return contact.hadCollision && !contact.discarded &&
                   std::abs(contact.position.x-100.f)<.1f && std::abs(contact.position.y-20.f)<.1f &&
                   std::abs(contact.contactNormal.y-std::cos(edgeAngle))<.01f;
            });
          require(nearThreshold,"edge fixture must exercise a capsule contact normal next to the walkable slope threshold");
          }
        const Tempest::Vec3 request(5.f*std::cos(approach.heading*radians),0,5.f*std::sin(approach.heading*radians));
        const auto traverse=[&](bool ascending) {
          float progress=state.position.x;
          uint64_t stalled=0, maximumStall=0;
          int frames=0;
          for(;frames<180 && (ascending ? state.position.x<350.f : state.position.x>20.f);++frames) {
            state=walker->move(ascending?request:request*-1.f,17,50,slope,80,nullptr);
            if((ascending ? state.position.x-progress : progress-state.position.x)>=1.f) {
              progress=state.position.x;
              stalled=0;
              } else {
              stalled+=17;
              maximumStall=std::max(maximumStall,stalled);
              }
            if(stalled>1000) {
              std::fprintf(stderr,"CharacterVirtual stairs %s %s stalled: feet (%.4f,%.4f,%.4f), normal (%.4f,%.4f,%.4f)\n",
                           approach.name,ascending?"up":"down",double(state.position.x),double(state.position.y),double(state.position.z),
                           double(state.groundNormal.x),double(state.groundNormal.y),double(state.groundNormal.z));
              require(false,"twenty-centimetre stairs must never block a requested approach for more than one second");
              }
            }
          std::printf("CharacterVirtual stairs %s %s: frames %d, max stall %llu ms, feet (%.4f,%.4f,%.4f) cm\n",
                      approach.name,ascending?"up":"down",frames,static_cast<unsigned long long>(maximumStall),
                      double(state.position.x),double(state.position.y),double(state.position.z));
          require(state.grounded && (ascending ? state.position.x>=350.f : state.position.x<=20.f),
                  "virtual walking must finish twenty-centimetre stairs from every side, heading and threshold-edge approach");
          requireNear(state.position.y,ascending?60.f:0.f,2.f,"virtual capsule feet must finish on the expected stair landing");
          };
        traverse(true);
        traverse(false);
        }
      auto hero=world.createVirtualCharacter(25,180,{0,0,0});
      CollisionWorld::CharacterMoveResult state;
      auto wall=world.addCollisionBody(new JPH::BoxShape(JPH::Vec3(.1f,3,10)),at(600,300,0),.7f,
                                       DynamicWorld::C_Object);
      for(int i=0;i<150;++i)
        state=hero->move({5,0,5},17,50,slope,80,nullptr);
      printState("wall-slide-end",150,state);
      require(state.position.x<567 && state.position.z>650,
              "virtual walking must stop at the wall while retaining motion along it");
      hero->setPosition({-300,0,0});
      state=hero->move({},17,50,slope,80,nullptr);
      printState("teleport",1,state);
      requireNear(state.position.x,-300,.01f,"a virtual teleport must not retain old ground displacement");
      require(state.grounded,"a teleported virtual character must refresh its support");
    }
    auto push=[&](float mass, int category) {
      CollisionWorld world;
      auto floor=world.addCollisionBody(new JPH::BoxShape(JPH::Vec3(20,.5f,20)),at(0,-50,0),.7f,
                                        DynamicWorld::C_Landscape);
      auto prop=world.addDynamicBody(new JPH::BoxShape(JPH::Vec3(.2f,.4f,.4f)),at(80,40,0),.7f,mass,category,true);
      auto hero=world.createVirtualCharacter(25,180,{0,0,0});
      CollisionWorld::CharacterMoveResult state;
      for(int i=0;i<60;++i) {
        state=hero->move({5,0,0},17,10,slope,80,nullptr);
        world.tick(17);
        }
      printState("push-end",60,state);
      std::fprintf(stderr,"CharacterVirtual pushed body: mass=%.3f kg x=%.3f cm\n",double(mass),double(prop->position().x));
      require(state.position.x<prop->position().x-30,
              "virtual contacts must not allow the hero through a dynamic hull");
      return prop->position().x-80.f;
      };
    const float light=push(1,DynamicWorld::C_Item);
    const float heavy=push(122,DynamicWorld::C_MovableVob);
    require(light>20 && heavy<light*.5f,"the same character strength must push a light item faster than heavy furniture");
    std::printf("CharacterVirtual push: 1 kg %.2f cm, 122 kg %.2f cm\n",double(light),double(heavy));
    for(const int category:{DynamicWorld::C_Item,DynamicWorld::C_MovableVob}) {
      CollisionWorld world;
      auto floor=world.addCollisionBody(new JPH::BoxShape(JPH::Vec3(20,.5f,20)),at(0,-50,0),.7f,
                                        DynamicWorld::C_Landscape);
      auto mug=world.addDynamicBody(new JPH::BoxShape(JPH::Vec3(.07f,.08f,.07f)),at(80,8,0),.7f,5.8f,category,true);
      auto hero=world.createVirtualCharacter(25,180,{0,0,0});
      float highest=0.f;
      CollisionWorld::CharacterMoveResult state;
      for(int i=0;i<60;++i) {
        state=hero->move({5,0,0},17,50,slope,80,nullptr);
        highest=std::max(highest,state.position.y);
        world.tick(17);
        }
      std::printf("CharacterVirtual loose utensil: category %d moved %.3f cm, max feet %.3f cm\n",
                  category,double(mug->position().x-80.f),double(highest));
      require(highest<1.f,"walking into a loose item or small movable utensil must not lift the feet");
      require(mug->position().x>100.f && mug->touchedByNpc(),
              "a loose utensil excluded as a support must still receive native contact impulses");
      require(state.position.x>250.f,"a loose utensil must be pushed aside while the hero keeps walking");
    }
    {
      CollisionWorld world;
      auto mug=world.addDynamicBody(new JPH::BoxShape(JPH::Vec3(.2f,.08f,.2f)),at(0,8,0),.7f,5.8f,
                                     DynamicWorld::C_Item,true);
      auto hero=world.createVirtualCharacter(25,180,{0,16,0});
      CollisionWorld::CharacterMoveResult state;
      // Leave the rigid body suspended without ticking it: the feet can only
      // remain at 16 cm if this light body is erroneously accepted as a floor.
      for(int i=0;i<10;++i)
        state=hero->move({},17,50,slope,80,nullptr);
      require(!state.grounded && state.position.y<14.f,
              "even feet initially placed on a light item must fall rather than acquire it as support");
      requireNear(mug->velocity().y,0,.001f,"an excluded light support must not receive character support weight");
    }
    {
      CollisionWorld world;
      auto floor=world.addCollisionBody(new JPH::BoxShape(JPH::Vec3(20,.5f,20)),at(0,-50,0),.7f,
                                        DynamicWorld::C_Landscape);
      // A low table is deliberately within the 50 cm stair limit, including
      // the capsule's rounded toe. It must remain furniture rather than a step.
      auto table=world.addDynamicBody(new JPH::BoxShape(JPH::Vec3(.2f,.1f,.6f)),at(100,10,0),1.f,122.f,
                                       DynamicWorld::C_MovableVob,true);
      auto hero=world.createVirtualCharacter(25,180,{0,0,0});
      CollisionWorld::CharacterMoveResult state;
      float highest=0.f;
      for(int i=0;i<90;++i) {
        state=hero->move({5,0,0},17,50,slope,80,nullptr);
        highest=std::max(highest,state.position.y);
        world.tick(17);
        }
      std::printf("CharacterVirtual low table: hero %.3f cm, table %.3f cm, max feet %.3f cm\n",
                  double(state.position.x),double(table->position().x),double(highest));
      require(highest<1.f,"ordinary walking must neither auto-step nor roll the capsule up a movable table");
      require(state.position.x<table->position().x-35.f,
              "a table excluded from automatic stepping must still block the capsule");
    }
    {
      CollisionWorld world;
      // No floor or simulation tick: measured linear momentum comes only from
      // character contact impulses, without gravity, support weight or friction.
      // Two narrow bodies block the same capsule. Each can demand a full force
      // allowance; repeated contacts / stair probes must share the move budget.
      constexpr float mass=20.f,seconds=.1f;
      auto a=world.addDynamicBody(new JPH::BoxShape(JPH::Vec3(.1f,.9f,.1f)),at(50,190,-20),.4f,mass,
                                  DynamicWorld::C_MovableVob,true);
      auto b=world.addDynamicBody(new JPH::BoxShape(JPH::Vec3(.1f,.9f,.1f)),at(50,190,20),.4f,mass,
                                  DynamicWorld::C_MovableVob,true);
      auto hero=world.createVirtualCharacter(25,180,{0,100,0});
      hero->move({100,0,0},100,50,slope,80,nullptr);
      const float impulseA=a->velocity().length()*.01f*mass;
      const float impulseB=b->velocity().length()*.01f*mass;
      constexpr float totalLimit=450.f*seconds;
      constexpr float bodyLimit=450.f*80.f/(80.f+mass)*seconds;
      std::printf("CharacterVirtual shared push: %.3f + %.3f Ns, total limit %.3f Ns, per-body %.3f Ns\n",
                  double(impulseA),double(impulseB),double(totalLimit),double(bodyLimit));
      require(impulseA>1.f && impulseB>1.f,"the shared push fixture must exercise two native contact bodies");
      require(impulseA<=bodyLimit+.01f && impulseB<=bodyLimit+.01f,
              "all contacts of one body must share its mass-dependent walking effort");
      require(impulseA+impulseB<=totalLimit+.01f,
              "all bodies and repeated contacts must share one 450 newton walking budget per move");
    }
    {
      zenkit::ModelHierarchy hierarchy{};
      hierarchy.collision_bbox = {{-25,0,-25},{25,180,25}};
      Skeleton shape(asset::fromZenKit(hierarchy),nullptr,"VIRTUAL_NPC_CONTRACT");
      DynamicWorld world(nullptr,asset::WorldMesh{});
      auto hero=world.addGhostObj(&shape);
      auto npc=world.addGhostObj(&shape);
      hero.setPosition({0,0,0});
      npc.setPosition({100,0,0});
      world.tick(0);
      world.tick(0); // leave the other NPC in the frozen broad-phase list
      for(int i=0;i<30;++i)
        hero.moveVirtual({5,0,0},17,50,slope,80);
      require(hero.position().x<55,"virtual hero walking must retain existing NPC ellipsoid obstacles");
      requireNear(npc.position().x,100,0,"virtual hero contacts must never move another NPC");
      hero.setPosition({-200,50,30});
      hero.resetVirtual();
      const auto after=hero.moveVirtual({},0,50,slope,80);
      requireNear(after.position.x,-200,.001f,"controller recreation must retain the saved or teleported feet x");
      requireNear(after.position.y,50,.001f,"controller recreation must retain the saved or teleported feet y");
      requireNear(after.position.z,30,.001f,"controller recreation must retain the saved or teleported feet z");
    }
    {
      zenkit::ModelHierarchy hierarchy{};
      hierarchy.collision_bbox = {{-25,0,-25},{25,180,25}};
      Skeleton shape(asset::fromZenKit(hierarchy),nullptr,"VIRTUAL_NPC_PAIR_CONTRACT");
      DynamicWorld world(nullptr,movableFloor());
      auto left=world.addGhostObj(&shape);
      auto right=world.addGhostObj(&shape);
      left.setPosition({0,0,0});
      right.setPosition({200,0,0});
      world.tick(0);
      world.tick(0);
      for(int i=0;i<40;++i) {
        left.moveVirtual({5,0,0},17,50,slope,80);
        right.moveVirtual({-5,0,0},17,50,slope,80);
        require(right.position().x-left.position().x>=49.9f,
                "two virtual NPCs moving towards each other must never cross or interpenetrate");
        }
      require(left.position().x>60.f && right.position().x<140.f,
              "both virtual NPCs must advance before their mutual blocking contact");
      requireNear(left.position().y,0.f,1.f,"an NPC contact must not lift virtual feet onto another NPC");
      requireNear(right.position().y,0.f,1.f,"an opposing NPC contact must retain floor support");
      const auto stoppedRight=right.position();
      const auto blocked=left.moveVirtual({5,0,0},17,50,slope,80);
      require(blocked.blocked && blocked.collision.npcCol>0.f,
              "unsuccessful virtual NPC advancement must expose its blocking character contact to MoveAlgo");
      requireNear(right.position().x,stoppedRight.x,0.f,"a virtual NPC contact must never push its neighbour");
      const auto sway=left.moveVirtual({1.9f,0,0},17,50,slope,80);
      require(!sway.blocked,"a legacy-skipped sway into another NPC must not notify AI of movement failure");
      requireNear(right.position().x,stoppedRight.x,0.f,"a skipped sway must preserve the neighbouring NPC obstacle");
      right.setEnable(false);
      for(int i=0;i<20;++i)
        left.moveVirtual({5,0,0},17,50,slope,80);
      require(left.position().x>150.f,"a seated character with disabled collision must not leave a virtual obstacle");
    }
    {
      zenkit::ModelHierarchy hierarchy{};
      hierarchy.collision_bbox = {{-25,0,-25},{25,180,25}};
      Skeleton shape(asset::fromZenKit(hierarchy),nullptr,"VIRTUAL_NPC_OVERLAP_CONTRACT");
      DynamicWorld world(nullptr,movableFloor());
      auto walker=world.addGhostObj(&shape);
      auto overlapping=world.addGhostObj(&shape);
      auto clearNeighbour=world.addGhostObj(&shape);
      walker.setPosition({0,0,0});
      overlapping.setPosition({40,0,0});
      clearNeighbour.setPosition({-55,0,0});
      for(int i=0;i<4;++i)
        walker.moveVirtual({-5,0,0},17,50,slope,80);
      require(walker.position().x>=-5.01f && walker.position().x<-4.f,
              "separating a teleported overlap must not transfer penetration to a previously clear neighbour");
      requireNear(overlapping.position().x,40.f,0.f,"overlap recovery must not displace the original neighbour");
      requireNear(clearNeighbour.position().x,-55.f,0.f,"overlap recovery must not displace the clear neighbour");
    }
    {
      Resources resources;
      ProtoMesh wall(PackedMesh(movableCollisionBox({20,400,600}),PackedMesh::PK_Visual),"VIRTUAL_NPC_WALL.MRM");
      zenkit::ModelHierarchy hierarchy{};
      hierarchy.collision_bbox = {{-25,0,-25},{25,180,25}};
      Skeleton shape(asset::fromZenKit(hierarchy),nullptr,"VIRTUAL_NPC_WALL_CONTRACT");
      DynamicWorld world(nullptr,movableFloor());
      auto structure=world.addStaticObj(wall.attach[0].shape.get(),at(50,200,0),"VIRTUAL_NPC_WALL.MRM");
      auto walker=world.addGhostObj(&shape);
      auto obstacle=world.addGhostObj(&shape);
      walker.setPosition({0,0,0});
      obstacle.setPosition({15,0,150});
      // The diagonal desired motion misses the NPC, while wall sliding would
      // encounter it. The post-solve clip must retain a world-safe pose.
      const auto stopped=walker.moveVirtual({200,0,200},100,50,slope,80);
      require(!structure.isEmpty(),"virtual NPC wall fixture must install its static collision mesh");
      require(stopped.position.x<=15.1f && stopped.position.z<100.f,
              "a wall-resolved virtual move must not cross a character or interpolate feet into the wall");
      requireNear(obstacle.position().x,15.f,0.f,"wall sliding must not push a neighbouring NPC through the wall");
      requireNear(obstacle.position().z,150.f,0.f,"wall sliding must leave the neighbouring NPC at its own feet");
    }
    }

  void testVirtualFloorGuard() {
    constexpr float slope=.785398163f;
    zenkit::ModelHierarchy hierarchy{};
    hierarchy.collision_bbox={{-25,0,-25},{25,180,25}};
    Skeleton shape(asset::fromZenKit(hierarchy),nullptr,"VIRTUAL_FLOOR_GUARD_CONTRACT");
    auto ledge=movableFloor();
    for(auto& vertex:ledge.vertices)
      vertex.x=vertex.x>0 ? 0.f : vertex.x;
    for(bool lowerFloor:{false,true}) {
      auto mesh=ledge;
      if(lowerFloor) {
        auto lower=movableFloor();
        for(auto vertex:lower.vertices) {
          vertex.y=-500.f;
          mesh.vertices.push_back(vertex);
          }
        for(auto index:lower.polygons.vertex_indices)
          mesh.polygons.vertex_indices.push_back(index+4);
        mesh.polygons.material_indices.insert(mesh.polygons.material_indices.end(),{0,0});
        }
      DynamicWorld world(nullptr,mesh);
      auto walker=world.addGhostObj(&shape);
      walker.setPosition({-35,0,0});
      walker.moveVirtual({},17,50,slope,80);
      const auto start=walker.position();
      require(walker.hasFloorBelow(start),"the virtual floor guard fixture must start over supported world");
      require(walker.hasFloorBelow({45,80,20})==lowerFloor,
              "jump handoff must distinguish a bottomless lateral step from a lower landing floor");
      auto moved=walker.moveVirtual({80,0,20},100,50,slope,80);
      if(!lowerFloor) {
        require(moved.blocked && moved.collision.preFall,
                "a virtual step beyond all floor must report the legacy pre-fall failure");
        requireNear((moved.position-start).length(),0,.001f,
                    "a rejected bottomless virtual step must retain the known-safe feet");
        for(int i=0;i<180;++i)
          moved=walker.moveVirtual({5,0,1},17,50,slope,80);
        require(moved.position.x<=0.f && moved.position.y>-.5f,
                "repeated virtual steps must not enter a bottomless world boundary");
        } else {
        require(!moved.collision.preFall && moved.position.x>0.f,
                "a real drop with a lower floor must remain accessible to virtual walking");
        for(int i=0;i<180;++i)
          moved=walker.moveVirtual({0,moved.velocity.y*.017f,0},17,50,slope,80);
        require(moved.position.y<-450.f && moved.grounded,
                "virtual feet must be able to fall and land on a lower floor");
        }
      }
    }

  // Shadow bodies as they were published before owner slots: a linear find per
  // publication, erase_if per removal. Casts must see exactly these bodies, in
  // this order: equal-time contacts on the same bone keep it.
  struct ReferenceShadowBodies final {
    struct Body final {
      const Npc*                    owner = nullptr;
      std::vector<MeleeBodyCapsule> capsules;
      JPH::AABox                    bounds;
      };
    std::vector<Body> bodies;

    void update(const Npc* owner, const std::vector<MeleeBodyCapsule>& capsules) {
      JPH::AABox bounds;
      for(const auto& capsule:capsules) {
        const auto from=CollisionWorld::toMeters(capsule.from),to=CollisionWorld::toMeters(capsule.to);
        const auto radius=JPH::Vec3::sReplicate(capsule.radiusCm*.01f);
        bounds.Encapsulate(JPH::AABox(JPH::Vec3::sMin(from,to)-radius,JPH::Vec3::sMax(from,to)+radius));
        }
      auto it=std::find_if(bodies.begin(),bodies.end(),[owner](const Body& body) { return body.owner==owner; });
      if(it==bodies.end()) {
        bodies.push_back({owner,capsules,bounds});
        } else {
        it->capsules=capsules;
        it->bounds=bounds;
        }
      }

    void remove(const Npc* owner) {
      std::erase_if(bodies,[owner](const Body& body) { return body.owner==owner; });
      }
    };

  std::array<uint32_t,3> bitsOf(const Tempest::Vec3& v) {
    return {std::bit_cast<uint32_t>(v.x),std::bit_cast<uint32_t>(v.y),std::bit_cast<uint32_t>(v.z)};
    }

  std::array<uint32_t,3> bitsOf(JPH::Vec3Arg v) {
    return bitsOf(Tempest::Vec3(v.GetX(),v.GetY(),v.GetZ()));
    }

  bool sameShadowBody(const MeleeShadowBody& body, const ReferenceShadowBodies::Body& expected) {
    if(!body.valid || body.capsules.size()!=expected.capsules.size() ||
       bitsOf(body.boundsMin)!=bitsOf(expected.bounds.mMin) || bitsOf(body.boundsMax)!=bitsOf(expected.bounds.mMax))
      return false;
    for(size_t i=0; i<body.capsules.size(); ++i) {
      const auto& a=body.capsules[i];
      const auto& b=expected.capsules[i];
      if(bitsOf(a.from)!=bitsOf(b.from) || bitsOf(a.to)!=bitsOf(b.to) ||
         std::bit_cast<uint32_t>(a.radiusCm)!=std::bit_cast<uint32_t>(b.radiusCm) || a.bone!=b.bone || a.zone!=b.zone)
        return false;
      }
    return true;
    }

  // The published list equals the reference body for body and in order, and
  // every owner's slot leads to its own body; owners without one have no slot.
  void requireReferenceShadowBodies(const CollisionWorld& world, const ReferenceShadowBodies& reference,
                                    const std::vector<const Npc*>& owners, const char* message) {
    const auto published=world.shadowBodies();
    bool same=published.size()==reference.bodies.size();
    for(size_t i=0; same && i<published.size(); ++i)
      same=published[i].first==reference.bodies[i].owner && sameShadowBody(published[i].second,reference.bodies[i]);
    for(const auto* owner:owners) {
      MeleeShadowBody slot;
      const bool found=world.shadowBody(owner,slot);
      auto it=std::find_if(reference.bodies.begin(),reference.bodies.end(),[owner](const auto& body) { return body.owner==owner; });
      same=same && found==(it!=reference.bodies.end()) && (!found || sameShadowBody(slot,*it));
      }
    require(same,message);
    }

  std::vector<MeleeBodyCapsule> shadowCapsules(size_t owner, uint32_t pose) {
    // Two or three capsules, a different place for every owner and pose.
    std::vector<MeleeBodyCapsule> capsules;
    const float x=float(owner)*37.f+float(pose%7)*1.25f, z=float(pose%5)*-3.5f;
    for(size_t i=0; i<2+(owner+pose)%2; ++i)
      capsules.push_back({{x,90.f+float(i)*20.f,z},{x+float(i),110.f+float(i)*20.f,z+2.f},3.f+float(i),i+owner,uint8_t(1+i%3)});
    return capsules;
    }

  void testShadowBodySlots() {
    std::array<int,8> identities{};
    std::vector<const Npc*> owners;
    for(auto& identity:identities)
      owners.push_back(reinterpret_cast<const Npc*>(&identity));
    auto npc=[&](size_t i) { return const_cast<Npc*>(owners[i]); };

    {
      CollisionWorld world;
      ReferenceShadowBodies reference;
      auto publish=[&](size_t owner, uint32_t pose) {
        const auto capsules=shadowCapsules(owner,pose);
        world.updateShadowBody(npc(owner),capsules);
        reference.update(owners[owner],capsules);
        };
      auto remove=[&](size_t owner) {
        world.removeShadowBody(owners[owner]);
        reference.remove(owners[owner]);
        };
      for(size_t i=0; i<6; ++i)
        publish(i,0);
      requireReferenceShadowBodies(world,reference,owners,"first publications append bodies in order");
      for(size_t i=0; i<6; ++i)
        publish(i,1);
      requireReferenceShadowBodies(world,reference,owners,"republishing updates every body in its own slot");

      // removeShadowBody in the middle: the bodies behind it move down a slot.
      remove(2);
      for(size_t i=3; i<6; ++i)
        publish(i,2);
      requireReferenceShadowBodies(world,reference,owners,"a removed body leaves the later slots pointing at their owners");
      remove(2);
      remove(7);
      requireReferenceShadowBodies(world,reference,owners,"removing an owner without a body changes nothing");

      // A damage script in the middle of the shadow loop spawns one NPC and
      // removes two: one published already this frame, one not yet.
      publish(0,3);
      publish(1,3);
      publish(6,3);
      remove(4);
      remove(0);
      publish(3,3);
      publish(5,3);
      publish(6,4);
      requireReferenceShadowBodies(world,reference,owners,"spawn and removal mid-loop keep slots and order");

      // Publishing again after removal appends at the end, as before.
      publish(2,5);
      publish(0,5);
      publish(2,6);
      requireReferenceShadowBodies(world,reference,owners,"a body published again after removal returns at the end");

      // Any sequence: the slot table never disagrees with the linear search.
      std::mt19937 random(20261108);
      for(uint32_t step=0; step<4000; ++step) {
        const size_t owner=random()%owners.size();
        if(random()%4==0)
          remove(owner); else
          publish(owner,step);
        if(step%97==0)
          requireReferenceShadowBodies(world,reference,owners,"random publications and removals match the linear search");
        }
      requireReferenceShadowBodies(world,reference,owners,"random publications and removals match the linear search");
      }

    // NPCs publish posed bodies with a serial and their slot hint. The same
    // serial is not copied again; a hint gone stale after a removal, or a body
    // written by another path in between, still ends in the owner's own slot.
    {
      CollisionWorld world;
      ReferenceShadowBodies reference;
      std::array<MeleeShadowBody,4> posed;
      std::array<size_t,4> hints;
      hints.fill(size_t(-1));
      uint64_t serial=0;
      auto pose=[&](size_t owner, uint32_t at) {
        posed[owner].capsules=shadowCapsules(owner,at);
        CollisionWorld::boundShadowBody(posed[owner]);
        posed[owner].serial=++serial;
        };
      auto publish=[&](size_t owner) {
        world.updateShadowBody(npc(owner),posed[owner],&hints[owner]);
        reference.update(owners[owner],posed[owner].capsules);
        };
      for(size_t i=0; i<4; ++i) {
        pose(i,0);
        publish(i);
        }
      require(hints==std::array<size_t,4>{0,1,2,3},"slot hints follow the publication order");
      publish(1);
      requireReferenceShadowBodies(world,reference,owners,"a serial published again keeps its body");
      world.removeShadowBody(owners[0]);
      reference.remove(owners[0]);
      pose(2,1);
      for(size_t i=1; i<4; ++i)
        publish(i);
      require(hints[1]==0 && hints[2]==1 && hints[3]==2,"hints stale after a removal are corrected");
      requireReferenceShadowBodies(world,reference,owners,"stale hints publish into the owners' slots");
      world.updateShadowBody(npc(3),shadowCapsules(3,9));
      reference.update(owners[3],shadowCapsules(3,9));
      publish(3);
      requireReferenceShadowBodies(world,reference,owners,"a body written in between is replaced by the posed serial");
      publish(0);
      require(hints[0]==3,"an owner published again after removal gets the last slot");
      requireReferenceShadowBodies(world,reference,owners,"an owner published again keeps its posed body");
      }

    // An NPC destroyed before another NPC's blade is flushed: its body is gone
    // from that cast, and contacts already recorded on it are dropped.
    for(const bool recorded:{false,true}) {
      CollisionWorld world;
      ReferenceShadowBodies reference;
      auto* attacker=npc(0);
      auto* first=npc(1);
      auto* second=npc(2);
      auto* third=npc(3);
      auto publish=[&](Npc* owner, const std::vector<MeleeBodyCapsule>& capsules) {
        world.updateShadowBody(owner,capsules);
        reference.update(owner,capsules);
        };
      publish(attacker,{{{0,0,-300},{0,20,-300},1.f,0,1}});
      publish(first,{{{-30,0,0},{-30,20,0},1.f,0,1}});
      publish(second,{{{30,0,0},{30,20,0},1.f,0,1}});
      publish(third,{{{0,0,50},{0,20,50},1.f,0,1}});
      const MeleeBladeSegment before{{-100,1,0},{-100,20,0},{1,0,0},2.f};
      const MeleeBladeSegment after{{100,1,0},{100,20,0},{1,0,0},2.f};
      world.updateShadowBlade(attacker,1,{before},0,0,100);
      world.updateShadowBlade(attacker,1,{after},100,0,100);
      if(recorded)
        world.flushShadowBlades();
      world.removeShadowBody(first);
      reference.remove(first);
      if(!recorded)
        world.flushShadowBlades();
      const auto hits=world.takeShadowBladeContacts(attacker);
      require(hits.size()==1 && hits[0].npc==second,
              recorded ? "a destroyed NPC's recorded contact is dropped before the attacker takes it"
                       : "a body removed before the flush is not cast against");
      requireReferenceShadowBodies(world,reference,owners,"removal during a swing keeps the other slots");
      // The next frame moves the survivors; a new swing finds them in their slots.
      publish(second,{{{60,0,0},{60,20,0},1.f,0,1}});
      publish(third,{{{0,0,2},{0,20,2},1.f,0,1}});
      requireReferenceShadowBodies(world,reference,owners,"survivors republish into their own slots");
      world.removeShadowBlade(attacker);
      world.updateShadowBlade(attacker,2,{before},200,200,300);
      world.updateShadowBlade(attacker,2,{after},300,200,300);
      world.flushShadowBlades();
      const auto next=world.takeShadowBladeContacts(attacker);
      require(next.size()==2 && next[0].npc==third && next[1].npc==second,
              "a swing after the removal hits the republished bodies where they moved");
      }
    }
  }

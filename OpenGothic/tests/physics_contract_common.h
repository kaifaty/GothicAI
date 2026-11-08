#pragma once

#include "asset/staticmesh.h"
#include "physics/dynamicworld.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace PhysicsContract {
  constexpr uint32_t landscapeMask = 1u << DynamicWorld::C_Landscape;
  constexpr uint32_t objectMask    = 1u << DynamicWorld::C_Object;
  constexpr uint32_t sensorMask    = 1u << DynamicWorld::C_Ghost;

  void require(bool condition, const char* message);
  void requireNear(float actual, float expected, float tolerance, const char* message);
  Tempest::Matrix4x4 at(float x, float y, float z);
  void requireMatrix(const Tempest::Matrix4x4& actual, const Tempest::Matrix4x4& expected,
                     const char* message);
  asset::StaticMesh movableCube();
  asset::StaticMesh movableBox(const Tempest::Vec3& dimensionsCm);
  asset::StaticMesh movableCollisionBox(const Tempest::Vec3& dimensionsCm);
  asset::WorldMesh movableFloor();
  std::vector<uint8_t> saveArchive(const std::function<void(Serialize&)>& write);
  void loadMovables(DynamicWorld& world, const std::vector<uint8_t>& bytes);
  asset::WorldMesh waterBasin(float floorY=-400.f, bool cave=false);
  void tickWater(DynamicWorld& world,int frames);
  const DynamicWorld::WaterBodySnapshot& waterBody(
      const std::vector<DynamicWorld::WaterBodySnapshot>& bodies,uint32_t zenId,size_t piece=size_t(-1));

  void testSphereQueryPrecision();
  void testSourceTerrainNormals();
  void testTerrainRayNormalScope();
  void testMeshRayEdges();
  void testCoincidentMaterials();
  void testSharedRayEdge();
  void testOriginalFloorProjection();
  void testShallowOverlapProjection();
  void testCompoundChunkSeams();

  void testNpcContactDistance();
  void testNpcFrozenBodyReplacement();
  void testNpcScalarWitness();
  void testNpcSeparatedContactResponse();
  void testCharacterVirtual();
  void testVirtualFloorGuard();
  void testShadowBodySlots();

  void testWaterBuoyancy();
  void testWaterDenseVobs();
  void testWaterShallowAndCave();
  void testItemWeaponWake();
  void testItemDeepestContact();
  void testItemWake();

  void testMovableVobImpulseInertia();
  void testMovableTableImpulseCouple();
  void testMovableVobGeometrySupport();
  void testMovableVobGeometryPenetration();
  void testMovableVobGeometryTopObjects();
  void testMovableVobGeometryAuthoredToggle();
  void testMovableVobClampedSavePose();
  void testMovableVobSave();
  void testMovableTraceThresholds();
  void testMovableVobArrowIdentity();
  void testDestruction();
  void testOwnFractureMotion();
  void testRotatingFragments();
  void testMovableVobLegacyCollision();
  void testMovableVobBodies();

  void testPhysicsNoise();
  void testRagdollNoise();
  void testBodyTraces();
  void testBodyImpactDamage();
  void testOffCenterImpulse();
  }

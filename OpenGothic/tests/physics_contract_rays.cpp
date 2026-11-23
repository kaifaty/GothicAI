#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "physics_contract_common.h"
#include "physics/collisionworld.h"
#include "physics/physicvbo.h"

#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastSphereVsTriangles.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace PhysicsContract {
  struct SpherePrecisionCollector final : JPH::CastShapeCollector {
    void AddHit(const JPH::ShapeCastResult& result) override {
      if(!hasHit || result.mFraction<hit.mFraction) {
        hit = result;
        hasHit = true;
        }
      }
    JPH::ShapeCastResult hit;
    bool hasHit = false;
    };

  SpherePrecisionCollector castSphereTriangle(const std::array<JPH::Vec3,3>& triangle,
                                              JPH::Vec3Arg start, JPH::Vec3Arg direction,
                                              float radius, const JPH::ShapeCastSettings& settings,
                                              JPH::Vec3Arg scale = JPH::Vec3::sOne(),
                                              JPH::Mat44Arg transform = JPH::Mat44::sIdentity(),
                                              float cutoff = 1.f+FLT_EPSILON) {
    JPH::SphereShape sphere(radius);
    const JPH::ShapeCast cast(&sphere,JPH::Vec3::sOne(),JPH::Mat44::sTranslation(start),direction);
    SpherePrecisionCollector collector;
    collector.ResetEarlyOutFraction(cutoff);
    JPH::CastSphereVsTriangles query(cast,settings,scale,transform,JPH::SubShapeIDCreator(),collector);
    query.Cast(triangle[0],triangle[1],triangle[2],0b111,JPH::SubShapeID());
    return collector;
    }

  void testSphereQueryPrecision() {
    JPH::ShapeCastSettings defaults;
    defaults.SetBackFaceMode(JPH::EBackFaceMode::CollideWithBackFaces);
    defaults.mActiveEdgeMode = JPH::EActiveEdgeMode::CollideWithAll;
    require(!defaults.mUsePreciseSphereTriangleCast,"native sphere triangle precision must remain an explicit opt-in");
    auto disabled = defaults;
    disabled.mUsePreciseSphereTriangleCast = false;
    auto precise = defaults;
    precise.mUsePreciseSphereTriangleCast = true;

    // Query 2179, with the actual decoded child-local input. The complete segment
    // misses this triangle by 2.54 mm beyond the 5 cm sphere radius. Float products
    // make the edge quadratic discriminant positive; the precise query rejects it.
    const std::array<JPH::Vec3,3> separatedEdge = {
      JPH::Vec3(-39.3174744f,-4.08496571f,15.2057877f),
      JPH::Vec3(-39.835434f,-2.79672146f,16.8955078f),
      JPH::Vec3(-38.9809036f,-3.76561832f,15.1747742f)
      };
    const JPH::Vec3 edgeStart(61.133728f,1.71995163f,12.8343124f);
    const JPH::Vec3 edgeDirection(-217.075928f,-11.5681381f,5.39838409f);
    const auto nativeEdge = castSphereTriangle(separatedEdge,edgeStart,edgeDirection,.049999997f,defaults);
    const auto explicitNativeEdge = castSphereTriangle(separatedEdge,edgeStart,edgeDirection,.049999997f,disabled);
    require(nativeEdge.hasHit && explicitNativeEdge.hasHit,
            "default native edge behavior must remain available to callers without the precision opt-in");
    require(nativeEdge.hit.mFraction==explicitNativeEdge.hit.mFraction &&
            nativeEdge.hit.mContactPointOn2==explicitNativeEdge.hit.mContactPointOn2 &&
            nativeEdge.hit.mPenetrationAxis==explicitNativeEdge.hit.mPenetrationAxis,
            "default and explicitly disabled precision must preserve the full native edge result");
    require(!castSphereTriangle(separatedEdge,edgeStart,edgeDirection,.049999997f,precise).hasHit,
            "a sphere segment separated from an edge must reject a rounded false discriminant");

    // Query 37252 is a very narrow face. Float Gram products report an interior
    // point while the entire segment is 60.5 cm beyond the one-meter radius.
    const std::array<JPH::Vec3,3> separatedThinFace = {
      JPH::Vec3(.549634933f,3.00050545f,64.0834045f),
      JPH::Vec3(1.74020004f,3.00050545f,59.6401825f),
      JPH::Vec3(3.03430557f,3.00050545f,54.8105469f)
      };
    const JPH::Vec3 thinStart(1.00256348f,17.6009464f,56.1907692f);
    const JPH::Vec3 thinDirection(0,-20.4721184f,0);
    require(castSphereTriangle(separatedThinFace,thinStart,thinDirection,1.f,defaults).hasHit,
            "default precision must preserve the native narrow-face result");
    require(!castSphereTriangle(separatedThinFace,thinStart,thinDirection,1.f,precise).hasHit,
            "a sphere cast outside a narrow face must reject rounded interior barycentrics");

    const std::array<JPH::Vec3,3> triangle = {
      JPH::Vec3(0,0,0),JPH::Vec3(0,0,2),JPH::Vec3(2,0,0)
      };
    const auto edge = castSphereTriangle(triangle,{-.25f,2,1},{0,-4,0},.5f,precise);
    require(edge.hasHit,"precise casts must retain a genuine edge contact");
    requireNear(edge.hit.mFraction,(2.f-std::sqrt(.5f*.5f-.25f*.25f))/4.f,0.000001f,
                "edge entry fraction must match the sphere cross section");
    const auto tangent = castSphereTriangle(triangle,{-.5f,2,1},{0,-4,0},.5f,precise);
    require(tangent.hasHit,"precise casts must retain exact edge tangencies");
    requireNear(tangent.hit.mFraction,.5f,0.000001f,"tangent contact must lie at the triangle plane");
    require(!castSphereTriangle(triangle,{-std::nextafter(.5f,1.f),2,1},{0,-4,0},.5f,precise).hasHit,
            "one representable step beyond the tangent must miss");
    const auto vertex = castSphereTriangle(triangle,{-.25f,2,-.25f},{0,-4,0},.5f,precise);
    require(vertex.hasHit,"precise casts must retain a genuine vertex contact");
    requireNear(vertex.hit.mFraction,(2.f-std::sqrt(.5f*.5f-2.f*.25f*.25f))/4.f,0.000001f,
                "vertex entry fraction must match the sphere cross section");

    // The game's sweep collector accepts fractions strictly below one.
    require(!castSphereTriangle(triangle,{.5f,2,.5f},{0,-1.5f,0},.5f,precise,
                               JPH::Vec3::sOne(),JPH::Mat44::sIdentity(),1.f).hasHit,
            "the precise query must respect an exclusive endpoint cutoff");
    const auto beforeEnd = castSphereTriangle(triangle,{.5f,2,.5f},{0,-1.5001f,0},.5f,precise,
                                              JPH::Vec3::sOne(),JPH::Mat44::sIdentity(),1.f);
    require(beforeEnd.hasHit && beforeEnd.hit.mFraction<1.f,
            "a real contact immediately before the endpoint must survive");
    require(!castSphereTriangle(triangle,{.5f,2,.5f},{0,-1.4999f,0},.5f,precise,
                               JPH::Vec3::sOne(),JPH::Mat44::sIdentity(),1.f).hasHit,
            "a contact beyond the segment endpoint must miss");

    const auto transform = JPH::Mat44::sRotationTranslation(
        JPH::Quat::sRotation(JPH::Vec3::sAxisY(),.5f*JPH::JPH_PI),JPH::Vec3(10,20,-30));
    const auto mirrored = castSphereTriangle(triangle,{-1,2,1.5f},{0,-4,0},.25f,precise,
                                             JPH::Vec3(-2,.5f,3),transform);
    require(mirrored.hasHit,"precise sphere queries must retain contacts with mirrored nonuniform meshes");
    requireNear(mirrored.hit.mFraction,.4375f,.000001f,"mesh scale must preserve the plane entry fraction");
    requireNear(mirrored.hit.mContactPointOn2.GetX(),11.5f,.00001f,"converted contact must preserve rotated world X");
    requireNear(mirrored.hit.mContactPointOn2.GetY(),20.f,.00001f,"converted contact must preserve world Y");
    requireNear(mirrored.hit.mContactPointOn2.GetZ(),-29.f,.00001f,"converted contact must preserve rotated world Z");
    }

  // These 12 original terrain faces reproduce all 107 logged Bullet normals.
  // Source log values used %.9g, which round-trip to the exact binary32 bits.
  void testSourceTerrainNormals() {
    struct Fixture {
      JPH::Vec3 vertices[3];
      uint32_t expected[3];
      };
    const Fixture fixtures[] = {
      { { {88.6986008f,5.17765617f,121.866676f},
          {91.4136581f,2.34256101f,112.210762f},
          {86.9217529f,5.54688644f,119.338539f} },
        { 0x3ece733du,0x3f67077eu,0xbe1b3b85u } }, // original source triangle 243603
      { { {86.9217529f,5.54688644f,119.338539f},
          {83.5601044f,7.12607956f,127.19075f},
          {88.6986008f,5.17765617f,121.866676f} },
        { 0x3e95ea55u,0x3f743014u,0xbd882751u } }, // original source triangle 243690
      { { {83.5601044f,7.12607956f,127.19075f},
          {86.9217529f,5.54688644f,119.338539f},
          {81.0413666f,7.49531031f,124.353905f} },
        { 0x3e78f42au,0x3f773e8au,0xbdb8a232u } }, // original source triangle 243717
      { { {86.9217529f,5.54688644f,119.338539f},
          {91.4136581f,2.34256101f,112.210762f},
          {86.245079f,5.06495762f,118.298805f} },
        { 0xbc2262c0u,0x3f68e0a7u,0xbed49493u } }, // original source triangle 243511
      { { {80.3646851f,7.01338053f,123.314186f},
          {86.9217529f,5.54688644f,119.338539f},
          {86.245079f,5.06495762f,118.298805f} },
        { 0xbd1a570cu,0x3f6a6f8cu,0xbeccc585u } }, // original source triangle 243513
      { { {86.245079f,3.88401794f,118.298828f},
          {79.8359985f,4.35840225f,123.528473f},
          {80.3646851f,5.83244085f,123.314186f} },
        { 0xbf1f942bu,0x3de37085u,0xbf4625b5u } }, // original source triangle 50088
      { { {79.8359985f,4.35840225f,123.528473f},
          {86.245079f,3.88401794f,118.298828f},
          {83.1608734f,3.53122926f,116.41391f} },
        { 0xbd08c995u,0x3f7da809u,0xbe05f250u } }, // original source triangle 243510
      { { {86.9217529f,5.54688644f,119.338539f},
          {80.3646851f,7.01338053f,123.314186f},
          {81.0413666f,7.49531031f,124.353905f} },
        { 0xbd1a59e8u,0x3f6a6f6du,0xbeccc606u } }, // original source triangle 243512
      { { {79.8359985f,4.35840225f,123.528473f},
          {83.1608734f,3.53122926f,116.41391f},
          {78.0155869f,3.55711174f,116.974243f} },
        { 0xbc02ca6au,0x3f7e2aefu,0xbdf40bdeu } }, // original source triangle 243504
      { { {83.1608734f,3.53122926f,116.41391f},
          {83.4825745f,3.9189806f,108.036804f},
          {78.0155869f,3.55711174f,116.974243f} },
        { 0x3c258252u,0x3f7fb563u,0x3d3ef6feu } }, // original source triangle 243502
      { { {78.0155869f,3.55711174f,116.974243f},
          {72.7686081f,1.26712513f,120.608086f},
          {72.7628021f,4.55114794f,122.417816f} },
        { 0xbf21fc20u,0x3ebee96du,0xbf2dbd0eu } }, // original source triangle 50059
      { { {72.7628021f,4.55114794f,122.417816f},
          {79.8359985f,4.35840225f,123.528473f},
          {78.0155869f,3.55711174f,116.974243f} },
        { 0x3d44e327u,0x3f7d615au,0xbe099467u } }, // original source triangle 243531
      };
    const std::vector<uint32_t> indices = {0,2,1}; // addIndex preserves Gothic winding by swapping 1/2.
    for(const auto& fixture:fixtures) {
      const std::vector<JPH::Vec3> vertices(fixture.vertices,fixture.vertices+3);
      PhysicVbo mesh(&vertices);
      mesh.addIndex(indices,0,3,asset::MaterialGroup::STONE);
      JPH::Vec3 normal;
      require(mesh.rayTriangleNormal(0,normal),"source terrain normal must be finite and nonzero");
      for(JPH::uint axis=0;axis<3;++axis)
        require(std::bit_cast<uint32_t>(normal[axis])==fixture.expected[axis],
                "source terrain normal must retain original Bullet float bits");
      require(!mesh.rayTriangleNormal(1,normal),"invalid source triangle must retain the native normal");
      }

    const float inf = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const std::vector<JPH::Vec3> invalid[] = {
      {{0,0,0},{0,0,0},{0,0,0}},
      {{0,0,0},{1,1,1},{2,2,2}},
      {{0,0,0},{1e-20f,0,0},{0,1e-20f,0}},
      {{0,0,0},{3e38f,0,0},{0,3e38f,0}},
      {{0,0,0},{inf,0,0},{0,1,0}},
      {{0,0,0},{nan,0,0},{0,1,0}},
      };
    for(const auto& vertices:invalid) {
      PhysicVbo mesh(&vertices);
      mesh.addIndex(indices,0,3,asset::MaterialGroup::STONE);
      JPH::Vec3 normal(7,8,9);
      require(!mesh.rayTriangleNormal(0,normal),"unusable source planes must retain native output");
      require(normal==JPH::Vec3(7,8,9),"invalid source normal must not overwrite native output");
      }
    }

  void testTerrainRayNormalScope() {
    const std::vector<JPH::Vec3> vertices = {
      {88.6986008f,5.17765617f,121.866676f},
      {91.4136581f,2.34256101f,112.210762f},
      {86.9217529f,5.54688644f,119.338539f},
      };
    const std::vector<uint32_t> indices = {0,2,1};
    PhysicVbo mesh(&vertices);
    mesh.addIndex(indices,0,3,asset::MaterialGroup::STONE);
    CollisionWorld world;
    auto landscape = world.addCollisionBody(mesh.shape(),at(0,0,0),.75f,
                                            DynamicWorld::C_Landscape,false,false,&mesh);
    auto object = world.addCollisionBody(mesh.shape(),at(0,0,0),.75f,
                                         DynamicWorld::C_Object,false,false,&mesh);
    const auto from = CollisionWorld::toCentimeters(JPH::Vec3(87.7866058f,7.30819273f,120.217987f));
    const auto to = CollisionWorld::toCentimeters(JPH::Vec3(87.7866058f,2.80819273f,120.217987f));
    const auto closest = world.rayCast(from,to,landscapeMask);
    JPH::Vec3 expected;
    require(mesh.rayTriangleNormal(0,expected),"terrain scope fixture must have a valid source normal");
    require(closest.hasHit && closest.body==landscape.get(),"identity terrain ray must hit its source face");
    const float normal[] = {closest.normal.x,closest.normal.y,closest.normal.z};
    for(JPH::uint axis=0;axis<3;++axis)
      require(std::bit_cast<uint32_t>(normal[axis])==std::bit_cast<uint32_t>(expected[axis]),
              "closest identity terrain ray must export the source plane normal");
    std::vector<CollisionWorld::QueryResult> all;
    world.rayCast(from,to,landscapeMask,false,&all);
    const auto objectHit = world.rayCast(from,to,objectMask);
    require(all.size()==1 && objectHit.hasHit && objectHit.body==object.get(),
            "native all-hit and object controls must hit the same geometry");
    const float native[] = {all[0].normal.x,all[0].normal.y,all[0].normal.z};
    const float objectNormal[] = {objectHit.normal.x,objectHit.normal.y,objectHit.normal.z};
    for(size_t axis=0;axis<3;++axis)
      require(std::bit_cast<uint32_t>(native[axis])==std::bit_cast<uint32_t>(objectNormal[axis]),
              "all-hit terrain and object queries must retain native normals");
    }

  void testMeshRayEdges() {
    CollisionWorld world;
    JPH::TriangleList triangles;
    triangles.emplace_back(JPH::Vec3(0,0,0),JPH::Vec3(0,0,1),JPH::Vec3(1,0,0));
    // This anchor admits the ray through the body's broad phase without hitting it.
    triangles.emplace_back(JPH::Vec3(-2,0,2),JPH::Vec3(-2,0,3),JPH::Vec3(-1,0,2));
    for(int i=1;i<=14;++i) {
      const float offset = float(i)*2.f;
      triangles.emplace_back(JPH::Vec3(offset,0,offset),JPH::Vec3(offset,0,offset+1),
                             JPH::Vec3(offset+1,0,offset));
      }
    JPH::MeshShapeSettings meshSettings(triangles);
    meshSettings.mMaxTrianglesPerLeaf = 1;
    const auto mesh = meshSettings.Create();
    require(!mesh.HasError(),"mesh ray edge test must construct its 16-leaf BVH");
    auto body = world.addCollisionBody(mesh.Get(),at(0,0,0),.75f,DynamicWorld::C_Landscape);

    // The target leaf has x >= 0; this point lies just outside that triangle with
    // one barycentric weight -5e-5. Both the edge test and BVH traversal need tolerance.
    const JPH::RayCast ray(JPH::Vec3(-5.0e-5f,1,.5f),JPH::Vec3(0,-2,0));
    const auto planePoint = ray.GetPointOnRay(.5f);
    JPH::Vec3 boundsMin, boundsMax;
    body->getAabb(boundsMin,boundsMax);
    require(JPH::AABox(boundsMin,boundsMax).Contains(planePoint),
            "anchor geometry must put the edge ray inside the overall body bounds");

    JPH::RayCastSettings exactSettings;
    JPH::ClosestHitCollisionCollector<JPH::CastRayCollector> exact;
    mesh.Get()->CastRay(ray,exactSettings,JPH::SubShapeIDCreator(),exact);
    require(!exact.HadHit(),"native default mesh rays must retain the exact triangle edge test");

    JPH::RayCastSettings compatibleSettings;
    compatibleSettings.mTriangleEdgeTolerance = 1.0e-4f;
    JPH::ClosestHitCollisionCollector<JPH::CastRayCollector> compatible;
    mesh.Get()->CastRay(ray,compatibleSettings,JPH::SubShapeIDCreator(),compatible);
    require(compatible.HadHit(),"mesh edge tolerance must reach triangles beyond their BVH leaf bounds");
    requireNear(compatible.mHit.mFraction,.5f,.000001f,"tolerant native mesh ray must hit the triangle plane");

    const auto gameHit = world.rayCast(CollisionWorld::toCentimeters(ray.mOrigin),
                                     CollisionWorld::toCentimeters(ray.GetPointOnRay(1)),landscapeMask);
    require(gameHit.hasHit && gameHit.body==body.get(),
            "game mesh rays must opt into the previous backend's edge tolerance");
    requireNear(gameHit.point.y,0,.001f,"compatible game mesh ray must report the triangle plane");
    require(!world.rayCast({-.02f,100,50},{-.02f,-100,50},landscapeMask).hasHit,
            "game mesh edge tolerance must reject points beyond its barycentric allowance");
    {
      const std::vector<JPH::Vec3> vertices={{-12,0,-9},{-12,0,9},{12,0,9},{12,0,-9}};
      PhysicVbo source(&vertices);
      source.addIndex({0,2,1},0,3,asset::MaterialGroup::WOOD,"first-large-face");
      source.addIndex({0,3,2},0,3,asset::MaterialGroup::STONE,"second-large-face");
      const auto split=source.shape(true);
      require(split!=nullptr && source.shape()->GetStats().mNumTriangles==2,
              "static subdivision must leave the original query and furniture mesh unchanged");
      require(source.triangleCount()==2 && source.vertexCount()==4,
              "static subdivision must preserve exact source vertices and triangle metadata");
      JPH::Shape::GetTrianglesContext context;
      split->GetTrianglesStart(context,split->GetLocalBounds(),JPH::Vec3::sZero(),
                               JPH::Quat::sIdentity(),JPH::Vec3::sOne());
      JPH::Float3 points[96];
      size_t count=0;
      float longest=0;
      for(int batch=split->GetTrianglesNext(context,32,points);batch>0;batch=split->GetTrianglesNext(context,32,points)) {
        count+=size_t(batch);
        for(int t=0;t<batch;++t) {
          JPH::Vec3 triangle[3];
          for(int v=0;v<3;++v)triangle[v]=JPH::Vec3(points[3*t+v].x,points[3*t+v].y,points[3*t+v].z);
          require((triangle[1]-triangle[0]).Cross(triangle[2]-triangle[0]).GetY()>0.f,
                  "static subdivision must preserve source face winding");
          for(int edge=0;edge<3;++edge)longest=std::max(longest,(triangle[(edge+1)%3]-triangle[edge]).Length());
          }
        }
      require(count>2 && longest<=4.0001f,"every native static collision edge must be at most four metres");
      auto support=world.addCollisionBody(split,at(0,0,0),.7f,DynamicWorld::C_Object,false,false,&source);
      const auto first=world.rayCast({-600,100,300},{-600,-100,300},objectMask);
      const auto second=world.rayCast({600,100,-300},{600,-100,-300},objectMask);
      require(first.hasHit && second.hasHit && first.body==support.get() && second.body==support.get() &&
              first.material==asset::MaterialGroup::WOOD && second.material==asset::MaterialGroup::STONE &&
              std::string(first.sector)=="first-large-face" && std::string(second.sector)=="second-large-face",
              "split collision faces must retain their original material, sector and exact ray plane");
      requireNear(first.point.y,0.f,.0001f,"split collision rays must retain the source plane");
      requireNear(second.point.y,0.f,.0001f,"split collision rays must retain the source plane");
      std::printf("Static subdivision: %zu triangles, longest edge %.6f m\n",count,double(longest));
      }
    }

  void testCoincidentMaterials() {
    CollisionWorld world;
    // Two pairs of cyclic copies from the original city mesh. Their float plane
    // fractions prefer wood for the first ray and stone for the second; a fixed
    // material or source-order preference therefore cannot preserve both cases.
    const std::vector<JPH::Vec3> vertices = {
      CollisionWorld::toMeters({6181.77686f,1425.00293f,8387.23633f}),
      CollisionWorld::toMeters({6415.78125f,1242.17615f,8519.63574f}),
      CollisionWorld::toMeters({6371.84814f,1237.50293f,8494.77344f}),
      CollisionWorld::toMeters({5791.45264f,1425.0033f,5789.07715f}),
      CollisionWorld::toMeters({5923.85352f,1242.17651f,5555.07324f}),
      CollisionWorld::toMeters({5898.98828f,1237.5033f,5599.00586f}),
      };
    const std::vector<uint32_t> wood = {2,1,0,5,4,3};
    const std::vector<uint32_t> stone = {0,2,1,3,5,4};
    PhysicVbo mesh(&vertices);
    mesh.addIndex(wood,0,wood.size(),asset::MaterialGroup::WOOD,"wood-face");
    mesh.addIndex(stone,0,stone.size(),asset::MaterialGroup::STONE,"stone-face");
    auto body = world.addCollisionBody(mesh.shape(),at(0,0,0),.75f,
                                       DynamicWorld::C_Landscape,false,false,&mesh);
    const Tempest::Vec3 from[] = {{6477.52686f,1279.26453f,7595.64355f},
                                 {3314.40405f,1255.13806f,5864.34229f}};
    const Tempest::Vec3 to[] = {{5786.84668f,1274.92175f,11801.4824f},
                               {65382.3594f,2494.34717f,376.108551f}};
    const uint32_t expected[] = {0,3};
    for(size_t i=0;i<2;++i) {
      require(mesh.hasRayDuplicates(uint32_t(i)) && mesh.hasRayDuplicates(uint32_t(i+2)),
              "cyclic faces must retain their original triangle IDs and duplicate groups");
      for(const auto triangle:{uint32_t(i),uint32_t(i+2)})
        require(mesh.resolveRayTriangle(triangle,CollisionWorld::toMeters(from[i]),
                                        CollisionWorld::toMeters(to[i]))==expected[i],
                "coincident ray metadata must use original float plane fractions");
      const auto hit = world.rayCast(from[i],to[i],landscapeMask);
      require(hit.hasHit && hit.body==body.get(),"coincident material ray must hit its native mesh");
      require(hit.material==mesh.materialId(expected[i]),"game rays must resolve coincident face materials");
      require(hit.sector!=nullptr && std::string(hit.sector)==mesh.sectorName(expected[i]),
              "game rays must resolve coincident face sectors with their materials");
      }
    require(mesh.materialId(0)==asset::MaterialGroup::WOOD && mesh.materialId(2)==asset::MaterialGroup::STONE,
            "non-ray callers must retain each original triangle's material");
    }

  void testSharedRayEdge() {
    CollisionWorld world;
    // Original city faces from ray 97350 share an edge. Compressed vertices put
    // the wood face first, while the original planes put the stone face first.
    const std::vector<JPH::Vec3> vertices = {
      {652.533203f,44.7002907f,149.346542f},
      {653.475281f,44.2155151f,154.1633f},
      {652.22406f,46.061451f,149.986694f},
      {648.160217f,48.0254517f,146.484192f},
      };
    PhysicVbo mesh(&vertices);
    mesh.addIndex({0,2,1},0,3,asset::MaterialGroup::WOOD,"first-native-face");
    mesh.addIndex({2,0,3},0,3,asset::MaterialGroup::STONE,"first-original-face");
    auto body = world.addCollisionBody(mesh.shape(),at(0,0,0),.75f,
                                       DynamicWorld::C_Landscape,false,false,&mesh);
    const Tempest::Vec3 from(64818.9414f,4486.26904f,15276.9766f);
    const Tempest::Vec3 to(68676.2578f,5028.1626f,12438.8008f);
    std::vector<CollisionWorld::QueryResult> all;
    const auto first = world.rayCast(from,to,landscapeMask,false,&all);
    require(all.size()==2 && first.body==body.get(),"shared-edge ray must retain both native face hits");
    require(all[0].material==asset::MaterialGroup::WOOD && all[1].material==asset::MaterialGroup::STONE &&
            all[0].fraction<all[1].fraction && first.fraction==all[0].fraction,
            "all-hit queries must preserve native fraction ordering at shared edges");
    const auto closest = world.rayCast(from,to,landscapeMask);
    require(closest.hasHit && closest.body==body.get() && closest.material==asset::MaterialGroup::STONE,
            "closest shared-edge ray must choose the valid nearest original plane");
    require(std::string(closest.sector)=="first-original-face",
            "shared-edge correction must keep the chosen native face's metadata");
    requireNear(closest.fraction,.108205222f,.00000001f,
                "closest mesh rays must export the chosen original plane fraction");
    requireNear(closest.normal.x,all[1].normal.x,.000001f,"shared-edge correction must retain the chosen native normal x");
    requireNear(closest.normal.y,all[1].normal.y,.000001f,"shared-edge correction must retain the chosen native normal y");
    requireNear(closest.normal.z,all[1].normal.z,.000001f,"shared-edge correction must retain the chosen native normal z");
    requireNear(closest.point.x,65236.3203f,.005f,"closest mesh rays must export the source-plane point x");
    requireNear(closest.point.y,4544.90479f,.001f,"closest mesh rays must export the source-plane point y");
    requireNear(closest.point.z,14969.8701f,.005f,"closest mesh rays must export the source-plane point z");

    // A ray through the first face's interior must keep its ordinary closest hit.
    const auto center = (vertices[0]+vertices[1]+vertices[2])/3.f;
    const auto normal = (vertices[1]-vertices[0]).Cross(vertices[2]-vertices[0]).Normalized();
    const auto interior = world.rayCast(CollisionWorld::toCentimeters(center+normal),
                                       CollisionWorld::toCentimeters(center-normal),landscapeMask);
    require(interior.hasHit && interior.material==asset::MaterialGroup::WOOD,
            "interior mesh rays must retain their closest native face");
    }

  void testOriginalFloorProjection() {
    CollisionWorld world;
    constexpr float height = 37.1234818f;
    const std::vector<JPH::Vec3> vertices = {
      {0,height,0},{0,height,1},{1,height,0},
      // Distant triangles widen the codec's bounds without intersecting the ray.
      {10,0,10},{11,0,10},{10,0,11},
      {10,100,10},{10,100,11},{11,100,10},
      };
    const std::vector<uint32_t> indices = {0,2,1,3,5,4,6,8,7};
    PhysicVbo mesh(&vertices);
    mesh.addIndex(indices,0,indices.size(),asset::MaterialGroup::STONE,"original-floor");
    auto body = world.addCollisionBody(mesh.shape(),at(0,0,0),.75f,
                                       DynamicWorld::C_Landscape,false,false,&mesh);
    const Tempest::Vec3 from(25,5000,25), to(25,-5000,25);
    std::vector<CollisionWorld::QueryResult> all;
    const auto native = world.rayCast(from,to,landscapeMask,false,&all);
    require(all.size()==1 && native.body==body.get(),"floor projection must retain its selected native face");
    require(std::abs(native.point.y-height*100.f)>.001f,
            "floor fixture must expose the compressed native height through the all-hit API");
    const auto closest = world.rayCast(from,to,landscapeMask);
    require(closest.hasHit && closest.body==body.get() && closest.material==asset::MaterialGroup::STONE,
            "source-plane projection must preserve the selected native body and material");
    requireNear(closest.point.y,height*100.f,.0005f,
                "interior floor rays must recover their original geometric height");
    requireNear(closest.point.x,25,.00001f,"floor projection must keep the ray's x coordinate");
    requireNear(closest.point.z,25,.00001f,"floor projection must keep the ray's z coordinate");
    requireNear(closest.normal.y,native.normal.y,0,"floor projection must preserve its native normal");
    requireNear(closest.fraction,.128765181f,.00000001f,
                "floor projection must export the fraction consistent with its original height");
    }

  void testShallowOverlapProjection() {
    CollisionWorld world;
    const Tempest::Vec3 from(71271.125f,3618.42529f,-12934.1797f);
    const Tempest::Vec3 to(66624.5391f,3618.77539f,-17199.3848f);
    // The sphere initially overlaps this small box and moves toward it, while
    // its center ray misses. The bottom projection extrapolates by about -286.
    auto body = world.addCollisionBody(new JPH::BoxShape(JPH::Vec3(.01f,.01f,.01f)),
                                       at(from.x-70,from.y,from.z+60),.75f,DynamicWorld::C_Landscape);
    const auto hit = world.raySweep(from,to,100,landscapeMask);
    require(hit.hasHit && hit.body==body.get(),"shallow initial overlap must retain its sphere hit");
    requireNear(hit.fraction,1,.000001f,"failed center ray must reset only the exported fraction");
    requireNear(hit.point.x,1399596.875f,.01f,"shallow overlap must preserve endpoint interpolation in x");
    requireNear(hit.point.y,3518.45703125f,.005f,"shallow overlap must preserve the projected bottom in y");
    requireNear(hit.point.z,1206365.625f,.01f,"shallow overlap must preserve endpoint interpolation in z");
    }

  void testCompoundChunkSeams() {
    // The routes and acceptance limits are the #616 edge/corner matrix. Only
    // ownership changes: four 64 m MeshShapes are children of one static body.
    constexpr uint32_t cells=16, side=2*cells+1;
    std::vector<JPH::Vec3> vertices;
    for(int x=-int(cells);x<=int(cells);++x)
      for(int z=-int(cells);z<=int(cells);++z)
        vertices.emplace_back(float(x)*4.f,0,float(z)*4.f);
    PhysicVbo mesh(&vertices);
    std::array<std::vector<uint32_t>,4> indices;
    const char* sectors[]={"south-west","north-west","south-east","north-east"};
    for(uint32_t x=0;x<2;++x)
      for(uint32_t z=0;z<2;++z) {
        auto& part=indices[x*2+z];
        for(uint32_t localX=0;localX<cells;++localX)
          for(uint32_t localZ=0;localZ<cells;++localZ) {
            const uint32_t a=(x*cells+localX)*side+z*cells+localZ,b=a+1,c=a+side+1,d=a+side;
            part.insert(part.end(),{a,c,b,a,d,c});
            }
        const auto material=(x+z)%2==0 ? asset::MaterialGroup::STONE : asset::MaterialGroup::WOOD;
        mesh.addIndex(part,0,part.size(),material,sectors[x*2+z]);
        }
    CollisionWorld whole,compoundWorld;
    auto wholeBody=whole.addCollisionBody(mesh.shape(true),at(0,0,0),0,DynamicWorld::C_Landscape,false,false,&mesh);
    JPH::StaticCompoundShapeSettings settings;
    for(uint32_t x=0;x<2;++x)
      for(uint32_t z=0;z<2;++z) {
        const auto part=x*2+z;
        const JPH::Vec3 origin(float(int(x)-1)*64.f,0,float(int(z)-1)*64.f);
        JPH::VertexList childVertices;
        JPH::IndexedTriangleList triangles;
        const auto& source=indices[part];
        for(uint32_t localX=0;localX<=cells;++localX)
          for(uint32_t localZ=0;localZ<=cells;++localZ)
            childVertices.emplace_back(float(localX)*4.f,0,float(localZ)*4.f);
        const auto localIndex=[&](uint32_t index) {
          return (index/side-x*cells)*(cells+1)+(index%side-z*cells);
          };
        // Match PhysicVbo::shape(true): split each 4 m square's long diagonal,
        // keep its original triangle userdata, and translate to a local origin.
        for(size_t i=0;i<source.size();i+=6) {
          const auto midpoint=(vertices[source[i]]+vertices[source[i+1]])*.5f-origin;
          const auto mid=uint32_t(childVertices.size());
          childVertices.emplace_back(midpoint.GetX(),midpoint.GetY(),midpoint.GetZ());
          const auto a=localIndex(source[i]),c=localIndex(source[i+1]),b=localIndex(source[i+2]),d=localIndex(source[i+4]);
          const auto triangle=part*2*cells*cells+uint32_t(i/3);
          triangles.emplace_back(c,mid,b,0,triangle);
          triangles.emplace_back(mid,a,b,0,triangle);
          triangles.emplace_back(a,mid,d,0,triangle+1);
          triangles.emplace_back(mid,c,d,0,triangle+1);
          }
        JPH::MeshShapeSettings child(std::move(childVertices),std::move(triangles));
        child.mPerTriangleUserData=true;
        const auto shape=child.Create();
        require(!shape.HasError(),"compound seam fixture must create each 64 m MeshShape");
        settings.AddShape(origin,JPH::Quat::sIdentity(),shape.Get().GetPtr());
        }
    const auto shape=settings.Create();
    require(!shape.HasError(),"compound seam fixture must create one static compound");
    auto compoundBody=compoundWorld.addCollisionBody(shape.Get(),at(0,0,0),0,DynamicWorld::C_Landscape,false,false,&mesh);
    require(compoundWorld.bodyCount()==1,"compound seam fixture must publish exactly one landscape body");
    // Speeds are representative walking/running inputs, not animation metadata.
    // Every route passes through its edge/corner; shallow approaches have enough
    // travel to clear the capsule on both sides without reaching an outer edge.
    constexpr uint64_t seamDt=17;
    constexpr float seamSeconds=float(seamDt)*.001f, halfRoute=600.f;
    size_t passedCount=0;
    std::puts("Compound seam matrix: kind speed_cm_s angle_deg height_mm vertical_m_s airborne stairs_attempted stairs_succeeded speed_loss_pct box_bounce_mm cv_travel_cm box_travel_cm result");
    for(const bool corner:{false,true})
      for(const float speed:{180.f,400.f})
        for(const float angle:{90.f,45.f,10.f}) {
          const float radians=angle*.01745329252f;
          const Tempest::Vec3 direction=angle==90.f ? Tempest::Vec3(1,0,0) :
                                                   Tempest::Vec3(std::sin(radians),0,std::cos(radians));
          const auto center=Tempest::Vec3(0,0,corner ? 0.f : -1600.f);
          const auto initial=center-direction*halfRoute;
          const auto displacement=direction*(speed*seamSeconds);
          const int frames=int(std::ceil(2.f*halfRoute/(speed*seamSeconds)));
          auto cv=compoundWorld.createVirtualCharacter(25,180,initial);
          auto reference=whole.createVirtualCharacter(25,180,initial);
          cv->setDiagnosticsEnabled(true);
          CollisionWorld::CharacterMoveResult state,expected;
          for(int frame=0;frame<30;++frame) {
            state=cv->move({},seamDt,50,.785398163f,80,nullptr);
            expected=reference->move({},seamDt,50,.785398163f,80,nullptr);
            }
          require(state.grounded && expected.grounded,"seam fixtures must settle on their flat support before measuring");
          const auto settled=state.position;
          float height=0,verticalSpeed=0,speedLoss=0;
          size_t airborne=0,stairsAttempted=0,stairsSucceeded=0;
          for(int frame=0;frame<frames;++frame) {
            const auto previous=state.position,previousExpected=expected.position;
            state=cv->move(displacement,seamDt,50,.785398163f,80,nullptr);
            expected=reference->move(displacement,seamDt,50,.785398163f,80,nullptr);
            CharacterVirtualDiagnostics diagnostic;
            require(cv->diagnostics(diagnostic),"seam measurements must retain native ground and stair diagnostics");
            height=std::max(height,std::abs(state.position.y-expected.position.y));
            // Include penetration recovery/stair snaps as actual vertical motion,
            // as well as the controller's solved velocity.
            verticalSpeed=std::max({verticalSpeed,std::abs(state.position.y-previous.y)/seamSeconds,
                                   std::abs(state.velocity.y)});
            const auto actualDelta=state.position-previous,expectedDelta=expected.position-previousExpected;
            const float actualAdvance=actualDelta.x*direction.x+actualDelta.z*direction.z;
            const float expectedAdvance=expectedDelta.x*direction.x+expectedDelta.z*direction.z;
            require(expected.grounded && expectedAdvance>0,"whole-body seam control must stay grounded and advance each frame");
            speedLoss=std::max(speedLoss,std::max(0.f,1.f-actualAdvance/expectedAdvance));
            airborne+=!state.grounded || diagnostic.afterStairs.groundState!=0 ? 1 : 0;
            stairsAttempted+=diagnostic.stairAttempted ? 1 : 0;
            stairsSucceeded+=diagnostic.stairSucceeded ? 1 : 0;
            }
          const auto cvDelta=state.position-settled;
          const float cvTravel=cvDelta.x*direction.x+cvDelta.z*direction.z;
          cv.reset();reference.reset();

          auto box=compoundWorld.addDynamicBody(new JPH::BoxShape({.1f,.1f,.1f}),at(initial.x,10,initial.z),0,1);
          auto control=whole.addDynamicBody(new JPH::BoxShape({.1f,.1f,.1f}),at(initial.x,10,initial.z),0,1);
          for(int frame=0;frame<120;++frame) {
            compoundWorld.tick(seamDt);whole.tick(seamDt);
            }
          const auto boxStart=box->position();
          box->setVelocity(direction*speed,{});
          control->setVelocity(direction*speed,{});
          float boxBounce=0;
          for(int frame=0;frame<frames;++frame) {
            compoundWorld.tick(seamDt);whole.tick(seamDt);
            boxBounce=std::max({boxBounce,box->position().y-boxStart.y,
                               std::abs(box->position().y-control->position().y)});
            }
          const auto boxDelta=box->position()-boxStart;
          const float boxTravel=boxDelta.x*direction.x+boxDelta.z*direction.z;
          const bool passed=height<=.5f && verticalSpeed<=10.f && airborne==0 && stairsSucceeded==0 &&
                            speedLoss<=.05f && cvTravel>halfRoute+25.f && boxBounce<=.5f && boxTravel>halfRoute+10.f;
          passedCount+=passed ? 1 : 0;
          std::printf("Compound seam %s %.0f %.0f %.6f %.6f %zu %zu %zu %.6f %.6f %.6f %.6f %s\n",
                      corner ? "corner" : "edge",double(speed),double(angle),double(height*10.f),double(verticalSpeed*.01f),
                      airborne,stairsAttempted,stairsSucceeded,double(speedLoss*100.f),double(boxBounce*10.f),
                      double(cvTravel),double(boxTravel),passed ? "PASS" : "FAIL");
          }

    std::printf("Compound seam experiment: %zu/12 passed (measurement only; not a production collision mode)\n",passedCount);
    }
  }

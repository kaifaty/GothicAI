#include "staticcollision.h"

#include "basestore.h"
#include "collisionworld.h"
#include "physicshost.h"
#include "physicvbo.h"
#include "resources.h"
#include "settings.h"
#include "worldcloth.h"
#include "graphics/mesh/submesh/packedmesh.h"

#include <chrono>
#include <cstdlib>
#include <exception>
#include <string_view>

StaticCollision::Cache StaticCollision::gameCache(const PhysicsHost& host, bool split) {
  Cache out;
  const auto* enabled=std::getenv("OPENGOTHIC_COLLISION_CACHE");
  if(enabled!=nullptr && std::string_view(enabled)=="0")
    return out;
  try {
    out.root=BaseStore::cacheRoot();
    }
  catch(const std::exception&) {
    // An unavailable derived cache must not prevent building the collision.
    }
  const auto inputs=Resources::wayHeightInputs(host.name());
  const auto& flags=Settings::physicsFlags();
  out.key={inputs.baseFingerprint,std::filesystem::path(host.name()).stem().string(),inputs.contentFingerprint,
           uint32_t(flags.cloth)|(uint32_t(flags.movableVobs)<<1)|(uint32_t(split)<<2),clothRules()};
  return out;
  }

std::string StaticCollision::clothRules() {
  // WorldCloth::extract takes these triangles out of the landscape with cloth on or off (cloth off adds them back
  // as a body of their own), so the rules are an input of the static collision either way.
  static constexpr char digits[]="0123456789abcdef";
  std::string out;
  for(const auto byte:WorldCloth::rulesSha256()) {
    out+=digits[byte>>4];
    out+=digits[byte&15];
    }
  return out+"/"+std::to_string(WorldCloth::Algorithm);
  }

StaticCollision::StaticCollision(const Input& input, const Cache& cache, bool split) {
  using Clock=std::chrono::steady_clock;
  auto started=Clock::now();
  const auto elapsed=[&]() {
    const auto now=Clock::now();
    const auto milliseconds=std::chrono::duration<double,std::milli>(now-started).count();
    started=now;
    return milliseconds;
    };
  const CollisionCache file(cache.root,cache.key);
  stats.keyMs=elapsed();
  if(!file.path().empty()) {
    stats.cacheHit=file.read(*this);
    stats.readMs=elapsed();
    }
  if(stats.cacheHit)
    return;
  stats.inputCalled=true;
  const auto& mesh=input();
  elapsed(); // the input times itself (World's physics_static mark: input_ms)
  pack(mesh);
  landShape=land->shape(split);
  waterShape=water->shape();
  // shape() prepares the triangle data of a mesh it builds; an empty mesh is prepared here. Contact listeners may
  // query it concurrently once the bodies are published, so it is never left to the first query.
  land->prepareTriangleData();
  water->prepareTriangleData();
  stats.buildMs=elapsed();
  if(!file.path().empty()) {
    file.save(*this);
    stats.writeMs=elapsed();
    }
  }

StaticCollision::~StaticCollision() = default;

void StaticCollision::pack(const asset::WorldMesh& mesh) {
  PackedMesh pkg(mesh,PackedMesh::PK_Physic);
  sectors.resize(pkg.subMeshes.size());
  for(size_t i=0;i<sectors.size();++i)
    sectors[i] = pkg.subMeshes[i].material.name;

  vertices.resize(pkg.vertices.size());
  for(size_t i=0;i<pkg.vertices.size();++i) {
    auto v = pkg.vertices[i];
    vertices[i] = CollisionWorld::toMeters(Tempest::Vec3(v.pos[0],v.pos[1],v.pos[2]));
    }

  land .reset(new PhysicVbo(&vertices));
  water.reset(new PhysicVbo(&vertices));

  for(size_t i=0;i<pkg.subMeshes.size();++i) {
    auto& sm = pkg.subMeshes[i];
    if(!sm.material.disable_collision && sm.iboLength>0) {
      if(sm.material.group==asset::MaterialGroup::WATER) {
        water->addIndex(pkg.indices,sm.iboOffset,sm.iboLength,sm.material.group);
        } else {
        land ->addIndex(pkg.indices,sm.iboOffset,sm.iboLength,sm.material.group,sectors[i].c_str());
        }
      }
    }
  }

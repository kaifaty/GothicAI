#pragma once

#include <cstdio>
#include <cstdlib>
#include <unordered_map>

#include "physics/dynamicworld.h"
#include "settings.h"
#include "testbench.h"
#include "world/bullet.h"
#include "world/objects/npc.h"
#include "world/world.h"

// Opt-in flight evidence; observes the queries and callbacks used by the actual
// projectile instead of issuing extra collision tests or consuming random draws.
namespace ArrowTrace {
struct Output {
  Output() {
    const auto* path=std::getenv("OPENGOTHIC_ARROW_TRACE");
    if(path!=nullptr && path[0]!='\0')
      file=std::fopen(path,"w");
    if(file!=nullptr)
      std::fputs("id,step,game_ms,phase,dt,x,y,z,dx,dy,dz,path_cm,bounces,range_cm,spell,precise,stopped,origin,target,target_x,target_y,target_z,npc,npc_x,npc_y,npc_z,has_hit,fraction,hit_x,hit_y,hit_z,nx,ny,nz,material,category,reflect_threshold,hit_limit,path_limit,living_bodies,arrow_physics,arrow_sticking\n",file);
    }
  ~Output() { if(file!=nullptr) std::fclose(file); }
  std::FILE* file=nullptr;
  uint64_t next=0;
  std::unordered_map<const void*,uint64_t> ids;
  };

inline Output& output() {
  static Output value;
  return value;
  }

struct Writer final : DynamicWorld::BulletTrace {
void forget(const DynamicWorld::BulletBody& body) override {
  auto& out=output();
  if(out.file!=nullptr) out.ids.erase(&body);
  }

void write(const DynamicWorld::BulletBody& body, DynamicWorld::BulletCallback* callback,
           const char* phase, uint64_t dt, const void* npcPtr,
           bool hasHit, float fraction,
           Tempest::Vec3 hit, Tempest::Vec3 normal, unsigned material, unsigned category) override {
  auto& out=output();
  if(out.file==nullptr) return;
  const auto* npc=static_cast<const Npc*>(npcPtr);
  const auto* bullet=dynamic_cast<const Bullet*>(callback);
  auto* origin=bullet!=nullptr ? bullet->origin() : nullptr;
  const auto* target=origin!=nullptr ? origin->target() : nullptr;
  auto [entry,inserted]=out.ids.try_emplace(&body,0);
  if(inserted) entry->second=++out.next;
  const auto p=body.position(), d=body.direction();
  const auto tp=target!=nullptr ? target->collosionCenter() : Tempest::Vec3{};
  const auto np=npc!=nullptr ? npc->collosionCenter() : Tempest::Vec3{};
  const auto by=TestBench::who(origin), at=TestBench::who(target), who=TestBench::who(npc);
  std::fprintf(out.file,"%llu,%llu,%llu,%s,%llu,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%d,%.9g,%d,%d,%d,%.*s,%.*s,%.9g,%.9g,%.9g,%.*s,%.9g,%.9g,%.9g,%d,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%u,%u,%.9g,%d,%d,%d,%d,%d\n",
    static_cast<unsigned long long>(entry->second),static_cast<unsigned long long>(TestBench::stepCount()),
    static_cast<unsigned long long>(origin!=nullptr ? origin->world().tickCount() : 0),phase,
    static_cast<unsigned long long>(dt),double(p.x),double(p.y),double(p.z),
    double(d.x),double(d.y),double(d.z),double(body.pathLength()),body.hitCount(),double(body.targetRange()),
    int(body.isSpell()),int(callback!=nullptr && callback->isPrecise()),int(bullet!=nullptr && bullet->isFinished()),
    int(by.size()),by.data(),int(at.size()),at.data(),double(tp.x),double(tp.y),double(tp.z),
    int(who.size()),who.data(),double(np.x),double(np.y),double(np.z),int(hasHit),double(fraction),
    double(hit.x),double(hit.y),double(hit.z),double(normal.x),double(normal.y),double(normal.z),material,category,
    double(body.speed()*fraction),int(body.hitCount()>3),int(body.pathLength()>10000),
    Settings::getI("GAME","living_bodies"),Settings::getI("GAME","arrow_physics"),
    Settings::getI("GAME","arrow_sticking"));
  std::fflush(out.file);
  }
};

inline DynamicWorld::BulletTrace* hook() {
  const auto* path=std::getenv("OPENGOTHIC_ARROW_TRACE");
  if(path==nullptr || path[0]=='\0') return nullptr;
  static Writer value;
  return &value;
  }
}

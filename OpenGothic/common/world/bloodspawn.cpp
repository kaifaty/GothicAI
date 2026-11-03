#include "bloodspawn.h"

#include <algorithm>

#include <cctype>
#include <string>

#include "game/constants.h"
#include "game/gamescript.h"
#include "graphics/effect.h"
#include "world/objects/pfxemitter.h"
#include "physics/dynamicworld.h"
#include "world/objects/npc.h"
#include "world/world.h"
#include "gothic.h"

using namespace Tempest;

namespace {
bool contains(std::string_view s, std::string_view what) {
  std::string up(s);
  for(auto& c:up)
    c = char(std::toupper(static_cast<unsigned char>(c)));
  return up.find(what)!=std::string::npos;
  }

BloodStains::Surface surfaceOf(asset::MaterialGroup m) {
  if(m==asset::MaterialGroup::EARTH)
    return BloodStains::Surface::Earth;
  if(m==asset::MaterialGroup::SNOW)
    return BloodStains::Surface::Snow;
  return BloodStains::Surface::Hard;
  }

uint8_t variantOf(const Vec3& p, uint32_t salt) {
  uint32_t h = uint32_t(int32_t(p.x))*73856093u ^ uint32_t(int32_t(p.z))*19349663u ^ salt*83492791u;
  h ^= h>>13;
  h *= 0x5bd1e995u;
  return uint8_t((h>>16)&0xFF);
  }

// Blood particles at the wound, the guild's own emitter (vanilla PFX_BLOOD for people).
void particles(World& world, int32_t guild, const Vec3& wound) {
  const auto& gv = world.script().guildVal();
  std::string_view name = "PFX_BLOOD";
  if(guild>=0 && size_t(guild)<std::size(gv.blood_emitter) && !gv.blood_emitter[guild].empty())
    name = gv.blood_emitter[guild];
  if(Gothic::inst().loadParticleFx(name,true)==nullptr)
    return;
  Matrix4x4 m;
  m.identity();
  m.translate(wound);
  Effect e(PfxEmitter(world,name),"");
  e.setObjMatrix(m);
  e.setActive(true);
  world.runEffect(std::move(e));
  }
}

BloodSpawn::GuildBlood BloodSpawn::guildBlood(const World& world, int32_t guild) {
  GuildBlood ret;
  const auto& gv = world.script().guildVal();
  if(guild<0 || size_t(guild)>=std::size(gv.blood_disabled))
    return ret;
  if(gv.blood_disabled[guild]!=0)
    return ret;
  const std::string_view tex = gv.blood_texture[guild];
  if(tex.empty()) {
    // people use the engine's default red; a beast without a texture (skeletons) leaves none
    ret.decal = guild<int32_t(GIL_SEPERATOR_HUM);
    }
  else {
    ret.decal = true;
    if(contains(tex,"MEATBUG"))
      ret.blood = BloodStains::Blood::Yellow;
    else if(contains(tex,"ZOMBIE"))
      ret.blood = BloodStains::Blood::Gore;
    else if(contains(tex,"GOLEM"))
      ret.blood = BloodStains::Blood::Stone;
    }
  if(gv.blood_max_distance[guild]>0)
    ret.reachCm = float(gv.blood_max_distance[guild]);
  if(gv.blood_amount[guild]>0)
    ret.amount = float(gv.blood_amount[guild])/10.f;
  return ret;
  }

void BloodSpawn::onHit(World& world, Npc& victim, const Vec3& wound, const Vec3& dir, int32_t damage) {
  if(!Gothic::physicsFlags().blood || damage<=0)
    return;
  const int32_t guild = int32_t(victim.guild());
  const auto    gb    = guildBlood(world, guild);
  particles(world, guild, wound);
  if(!gb.decal || gb.blood==BloodStains::Blood::Stone)
    return;
  auto* phys = world.physic();
  if(phys==nullptr)
    return;

  // the drops fly on along the blow and fall: a wall in the way catches them, else the ground
  Vec3 d = Vec3(dir.x, 0, dir.z);
  if(d.quadLength()<1e-4f)
    d = Vec3(1,0,0);
  d = Vec3::normalize(d);
  const float reach = std::min(gb.reachCm, 60.f+float(damage));

  BloodStains::Stain s;
  s.kind     = BloodStains::Kind::Splash;
  s.blood    = gb.blood;
  s.radiusCm = splashRadius(damage, gb.amount);

  auto wall = phys->rayStatic(wound, wound+d*reach);
  if(wall.hasCol && std::abs(wall.n.y)<0.7f) {
    Vec3 n = Vec3::normalize(wall.n);
    if(Vec3::dotProduct(n,d)>0)
      n = -n;
    s.pos     = wall.v;
    s.normal  = n;
    s.dir     = d*0.5f + Vec3(0,-1,0); // runs along the blow and down the wall
    s.surface = surfaceOf(wall.mat);
    } else {
    const Vec3 above = wound + d*(reach*0.6f);
    auto ground = phys->rayStatic(above, above-Vec3(0,400,0));
    if(!ground.hasCol)
      return;
    Vec3 n = ground.n.quadLength()>1e-4f ? Vec3::normalize(ground.n) : Vec3(0,1,0);
    if(n.y<0)
      n = -n;
    s.pos     = ground.v;
    s.normal  = n;
    s.dir     = d;
    s.surface = surfaceOf(ground.mat);
    }
  s.indoor  = world.isIndoor(s.pos);
  s.variant = variantOf(s.pos, uint32_t(damage));
  world.addBloodStain(s);
  victim.setBloodHit(world.time().toInt(), wound);
  }

void BloodSpawn::onDead(World& world, Npc& body) {
  if(!Gothic::physicsFlags().blood)
    return;
  const int64_t now = world.time().toInt();
  if(body.bloodHitAt()<0 || now-body.bloodHitAt()>PoolAfterHitMs)
    return;
  const auto gb = guildBlood(world, int32_t(body.guild()));
  if(!gb.decal || gb.blood==BloodStains::Blood::Stone)
    return;
  auto* phys = world.physic();
  if(phys==nullptr)
    return;

  // Blood runs out of the wound and spreads past the body, which hides what lies under it: the pool
  // is centred beside the spine on the wound's side, level with the wound, tucked under the body's edge.
  const Vec3 trunk = body.centerPosition();
  const Vec3 wound = body.bloodWound();
  const Vec3 head  = body.mapBone("BIP01 HEAD");
  Vec3 axis = Vec3(head.x-trunk.x, 0, head.z-trunk.z);
  axis = axis.quadLength()>1.f ? Vec3::normalize(axis) : Vec3(1,0,0);
  const Vec3  side  = Vec3(axis.z, 0, -axis.x);
  const Vec3  rel   = wound-trunk;
  const float along = std::clamp(Vec3::dotProduct(rel,axis), -60.f, 60.f);
  const float sign  = Vec3::dotProduct(rel,side)<0 ? -1.f : 1.f;
  const float r0    = poolRadius(body.physicsMass(), BloodStains::Surface::Hard);
  const Vec3  c     = trunk + axis*along + side*(sign*(25.f+0.65f*r0));
  auto ground = phys->rayStatic(c+Vec3(0,50,0), c-Vec3(0,250,0));
  if(!ground.hasCol)
    return;
  Vec3 n = ground.n.quadLength()>1e-4f ? Vec3::normalize(ground.n) : Vec3(0,1,0);
  if(n.y<0)
    n = -n;

  BloodStains::Stain s;
  s.kind     = BloodStains::Kind::Pool;
  s.blood    = gb.blood;
  s.surface  = surfaceOf(ground.mat);
  s.pos      = ground.v;
  s.normal   = n;
  s.dir      = Vec3(1,0,0);
  s.radiusCm = poolRadius(body.physicsMass(), s.surface)*std::max(0.4f, std::min(gb.amount, 1.2f));
  s.indoor   = world.isIndoor(s.pos);
  s.variant  = variantOf(s.pos, 7);
  s.bodyId   = body.bodyTrace().id;
  world.addBloodStain(s);
  }

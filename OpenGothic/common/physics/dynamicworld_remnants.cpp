#include "dynamicworld_internal.h"

#include "testbench.h"

#include <chrono>

// Remnants of a broken prop take blows again (reports/physics/2026-11-destruction-bonds, second
// step). A remnant is a bond group of several pieces: it keeps a reserve of class health x its
// volume share x `remnant`, and a blow past it breaks the bonds still intact inside it by the same
// rule as the first break. A single piece is one part: it only moves.

DynamicWorld::BodyData* DynamicWorld::fragmentOf(uintptr_t fragment, uint32_t& group, size_t& pieces) const {
  if(fragment==0)
    return nullptr;
  // The pointer came from a query: it is trusted only as one of the live bodies of debris.
  const auto* body=reinterpret_cast<const CollisionWorld::CollisionBody*>(fragment);
  for(auto* obj:movableVobs->bodies)
    for(uint32_t g=0;g<obj->fragments.size();++g) {
      if(obj->fragments[g].get()!=body)
        continue;
      if(g>=obj->fragmentRemnant.size() || obj->fractureRule==nullptr || obj->movableShape->fracture==nullptr)
        return nullptr;
      pieces=size_t(std::count(obj->fragmentGroup.begin(),obj->fragmentGroup.end(),g));
      group=g;
      return obj;
      }
  return nullptr;
  }

DynamicWorld::BodyData* DynamicWorld::remnantOf(uintptr_t fragment, uint32_t& group) const {
  size_t pieces=0;
  auto* obj=fragmentOf(fragment,group,pieces);
  return pieces>=2 ? obj : nullptr;
  }

void DynamicWorld::damageRemnant(BodyData& obj, uint32_t group, float damage, float impulse, const Tempest::Vec3& point,
                                 FractureHit hit, const Tempest::Vec3& direction) {
  if(!movableVobs->destructionEnabled || obj.fractureRule==nullptr || obj.movableShape->fracture==nullptr ||
     group>=obj.fragments.size() || group>=obj.fragmentRemnant.size() || obj.fragments[group]==nullptr ||
     !std::isfinite(damage) || damage<=0.f)
    return;
  auto& remnant=obj.fragmentRemnant[group];
  if(remnant.pending)
    return;
  const auto& mesh=*obj.movableShape->fracture;
  double volume=0;
  size_t pieces=0;
  for(size_t i=0;i<obj.fragmentGroup.size() && i<mesh.pieces.size();++i)
    if(obj.fragmentGroup[i]==group) {
      volume+=mesh.pieces[i].volume;
      ++pieces;
      }
  if(pieces<2 || !(mesh.volume>0))
    return;
  // Damage adds up as on the intact prop, against the loosened remnant's smaller reserve.
  remnant.damage+=damage;
  const float reserve=obj.fractureRule->health*float(volume/mesh.volume)*movableVobs->fractureBonds.remnant;
  if(reserve-remnant.damage>.001f)
    return;
  remnant.pending=true;
  remnant.hit=hit;
  remnant.point=point;
  remnant.direction=direction;
  remnant.impulse=impulse;
  }

void DynamicWorld::breakRemnants(BodyData& obj) {
  // Each break renumbers the groups; the pending blows move with their groups.
  for(;;) {
    const auto next=std::find_if(obj.fragmentRemnant.begin(),obj.fragmentRemnant.end(),[](const auto& remnant) {
      return remnant.pending;
      });
    if(next==obj.fragmentRemnant.end())
      return;
    breakRemnant(obj,uint32_t(next-obj.fragmentRemnant.begin()));
    }
  }

void DynamicWorld::breakRemnant(BodyData& obj, uint32_t group) {
  const auto blow=obj.fragmentRemnant[group];
  obj.fragmentRemnant[group].pending=false;
  if(!movableVobs->destructionEnabled || obj.movableShape->fracture==nullptr || group>=obj.fragments.size() ||
     obj.fragments[group]==nullptr)
    return;
  const auto started=std::chrono::steady_clock::now();
  const auto& mesh=*obj.movableShape->fracture;
  const auto& old=*obj.fragments[group];
  const auto matrix=old.matrix();
  const auto velocity=old.velocity(),omega=old.angularVelocity(),center=old.centerOfMass();
  std::vector<uint32_t> members;
  for(uint32_t i=0;i<obj.fragmentGroup.size();++i)
    if(obj.fragmentGroup[i]==group) members.push_back(i);
  if(members.size()<2)
    return;
  // The blow in the frame of the pieces: the remnant's body stands in the pose they were cut in.
  auto inverse=matrix;
  inverse.inverse();
  auto local=blow.point;
  inverse.project(local);
  obj.fragmentBondBroken.resize(mesh.bonds.size(),true);
  fractureCut(mesh,movableVobs->fractureBonds,blow.hit,local,members,obj.fragmentBondBroken);
  const auto pieceGroup=fractureComponents(mesh,obj.fragmentBondBroken);
  const auto pieces=fractureMembers(pieceGroup);
  const auto groups=uint32_t(pieces.size());
  // Every other group keeps its body and state under its new number (groups are numbered by
  // their smallest piece); the pieces of the struck remnant get new bodies.
  std::vector<std::unique_ptr<CollisionWorld::DynamicBody>> fragments(groups);
  std::vector<int64_t> sleepTime(groups,-1);
  std::vector<bool> permanent(groups,false);
  std::vector<BodyData::Remnant> remnants(groups);
  std::vector<uint8_t> fresh(groups,0);
  for(size_t i=0;i<pieceGroup.size();++i) {
    const auto from=obj.fragmentGroup[i],to=pieceGroup[i];
    if(from==group) {
      fresh[to]=1;
      continue;
      }
    if(obj.fragments[from]!=nullptr)
      fragments[to]=std::move(obj.fragments[from]);
    sleepTime[to]=obj.fragmentSleepTime[from];
    permanent[to]=obj.fragmentPermanent[from];
    remnants[to]=obj.fragmentRemnant[from];
    }
  // The hoop filter spans all groups of the prop: every body takes the new table.
  const auto filter=mesh.version==3 ? fractureGroupFilter(mesh,pieceGroup,groups) : nullptr;
  for(uint32_t g=0;g<groups;++g) {
    if(fresh[g]) {
      auto body=addFragmentBody(*world,mesh,pieces[g],matrix,obj.movableMass,obj.movableShape->material,
                                obj.movableShape->friction,obj.visual,filter.GetPtr(),obj.fragmentCollisionGroup,g,&obj);
      launchFragment(*body,pieces[g],movableVobs->fractureMotion,center,velocity,omega,blow.point,blow.direction,mesh.version);
      world->passCharacterOverlaps(*body);
      fragments[g]=std::move(body);
      }
    else if(fragments[g]!=nullptr && filter!=nullptr)
      fragments[g]->setFracturePiece(filter.GetPtr(),obj.fragmentCollisionGroup,g);
    }
  obj.fragments=std::move(fragments); // the struck remnant's body leaves the world here
  obj.fragmentSleepTime=std::move(sleepTime);
  obj.fragmentPermanent=std::move(permanent);
  obj.fragmentRemnant=std::move(remnants);
  obj.fragmentGroup=pieceGroup;
  chooseLastingGroups(obj);
  obj.fragmentsWereSleeping=false;
  syncFragments(obj);
  obj.breakImpulse=blow.impulse;
  emitBreakNoise(obj,center);
  if(TestBench::isActive())
    TestBench::destructionBreak(reinterpret_cast<uintptr_t>(&obj),obj.fragmentGroup.size(),
                               float(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count()),true);
  }

bool DynamicWorld::meleeStoppedRemnant(const MeleeBladeContact& contact, float weaponDamage, float strength,
                                       FractureRule::Melee kind, Npc* attacker, std::vector<uintptr_t>& struck) {
  if(!movableVobs->destructionEnabled || !contact.world || !contact.stopsSwing ||
     !std::isfinite(contact.point.x) || !std::isfinite(contact.point.y) || !std::isfinite(contact.point.z))
    return false;
  uint32_t group=0;
  size_t pieces=0;
  auto* obj=fragmentOf(contact.fragment,group,pieces);
  if(obj==nullptr)
    return false;
  // One swing strikes a prop once, whole or broken: `struck` holds the prop's snapshot id.
  const auto id=reinterpret_cast<uintptr_t>(obj);
  if(std::find(struck.begin(),struck.end(),id)!=struck.end())
    return false;
  auto direction=contact.velocityCmS;
  if(!(direction.quadLength()>1.e-6f)) direction=contact.normal*-1.f;
  if(!(direction.quadLength()>1.e-6f)) return false;
  direction=Tempest::Vec3::normalize(direction);
  const float damage=obj->fractureRule->meleeDamage(kind,weaponDamage,strength);
  if(!std::isfinite(damage) || damage<=0.f)
    return false;
  // The same push as the intact prop takes from this blow. A single piece only moves: it is
  // one part and does not break further, nor does it count as a strike on the prop.
  const float impulse=damage*.02f;
  auto& body=*obj->fragments[group];
  body.setTraceCulprit(attacker);
  body.addImpulseAtPoint(direction*impulse,contact.point,attacker);
  if(pieces<2)
    return false;
  damageRemnant(*obj,group,damage,impulse,contact.point,FractureRule::hit(kind),direction);
  struck.push_back(id);
  return true;
  }

void DynamicWorld::arrowRemnant(uintptr_t fragment, const Tempest::Vec3& point, const Tempest::Vec3& direction, Npc* culprit) {
  uint32_t group=0;
  auto* obj=movableVobs->destructionEnabled ? remnantOf(fragment,group) : nullptr;
  if(obj==nullptr)
    return;
  // Direction is cm/ms: a 120 gram arrow transfers SI momentum at its flight speed.
  const auto impulse=direction*1.2f;
  auto& body=*obj->fragments[group];
  body.setTraceCulprit(culprit);
  body.addImpulseAtPoint(impulse,point,culprit);
  const float ns=impulse.length();
  damageRemnant(*obj,group,ns>=.5f ? ns*obj->fractureRule->arrowDamage : 0.f,ns,point,FractureHit::Arrow,direction);
  }

size_t DynamicWorld::spellRemnants(BodyData& obj, const Tempest::Vec3& center, float radius,
                                   const std::function<Tempest::Vec3(const Tempest::Vec3&,float)>& impulse,
                                   Npc* culprit, const Tempest::Vec3* pointCm) {
  if(!movableVobs->destructionEnabled || obj.fractureRule==nullptr || obj.movableShape->fracture==nullptr)
    return 0;
  // A single piece keeps its present behaviour; a remnant takes the force and its damage.
  const auto members=fractureMembers(obj.fragmentGroup);
  size_t count=0;
  for(uint32_t g=0;g<obj.fragments.size() && g<members.size();++g) {
    auto* body=obj.fragments[g].get();
    if(body==nullptr || members[g].size()<2)
      continue;
    const auto target=body->centerOfMass();
    if((target-center).quadLength()>radius*radius)
      continue;
    const float mass=body->mass();
    const auto force=impulse(target,mass);
    if(force.quadLength()==0.f || !(mass>0.f) || world->rayCast(center,target,visualMask,false).hasHit)
      continue;
    if(!body->addImpulse(force,culprit,pointCm))
      continue;
    ++count;
    const auto& rule=*obj.fractureRule;
    damageRemnant(obj,g,std::max(force.length()/mass-rule.threshold,0.f)*rule.impactDamage,force.length(),
                  pointCm!=nullptr ? *pointCm : target,FractureHit::Spell,force);
    }
  return count;
  }

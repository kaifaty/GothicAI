#include "dynamicworld_internal.h"

#include "game/serialize.h"
#include "game/physicssave.h"
#include "testbench.h"

#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>

#include <chrono>
#include <limits>

// Debris of a broken prop: its bond groups as native bodies, their motion, sync, cleanup and
// save (reports/physics/2026-11-destruction-bonds).

namespace DynamicWorldInternal {
// The pieces of each bond group of one break, in piece order; groups by index.
std::vector<std::vector<uint32_t>> fractureMembers(const std::vector<uint32_t>& pieceGroup) {
  std::vector<std::vector<uint32_t>> members;
  for(uint32_t i=0;i<pieceGroup.size();++i) {
    if(pieceGroup[i]>=members.size()) members.resize(pieceGroup[i]+1);
    members[pieceGroup[i]].push_back(i);
    }
  return members;
  }

// The native body of one bond group of a break, in the parent's pose. One piece keeps its own
// hull; several pieces share a compound of their hulls (pieces are in the visual's frame, so
// without offsets). Mass is the sum of the pieces' volume shares; friction, sound and buoyancy
// follow the material with the largest volume in the group.
std::unique_ptr<CollisionWorld::DynamicBody> addFragmentBody(CollisionWorld& world, const FractureMesh& mesh,
                                                             const std::vector<uint32_t>& members,
                                                             const Tempest::Matrix4x4& matrix, float parentMass,
                                                             asset::MaterialGroup parentMaterial, float parentFriction,
                                                             const std::string& visual, const JPH::GroupFilter* filter,
                                                             uint32_t breakGroup, uint32_t group, void* parent) {
  float mass=0;
  std::vector<std::pair<asset::MaterialGroup,double>> volumes;
  for(const auto i:members) {
    const auto& piece=mesh.pieces[i];
    mass+=float(parentMass*piece.volume/mesh.volume);
    auto found=std::find_if(volumes.begin(),volumes.end(),[&](const auto& entry) { return entry.first==piece.group; });
    if(found==volumes.end()) volumes.emplace_back(piece.group,piece.volume);
    else found->second+=piece.volume;
    }
  auto dominant=volumes.front();
  for(const auto& entry:volumes)
    if(entry.second>dominant.second) dominant=entry;
  const auto material=mesh.version==3 ? dominant.first : parentMaterial;
  const float friction=mesh.version==3 ? DynamicWorld::materialFriction(material) : parentFriction;
  JPH::ShapeRefC shape=mesh.pieces[members.front()].hull;
  if(members.size()>1) {
    JPH::StaticCompoundShapeSettings compound;
    for(const auto i:members)
      for(const auto& hull:mesh.pieces[i].hulls)
        compound.AddShape(JPH::Vec3::sZero(),JPH::Quat::sIdentity(),hull);
    const auto result=compound.Create();
    if(result.HasError()) Tempest::Log::e("fracture: bond group of ",visual,": ",result.GetError().c_str());
    else shape=result.Get();
    }
  auto body=world.addDynamicBody(shape,matrix,friction,mass,DynamicWorld::C_Fragment);
  if(filter!=nullptr) body->setFracturePiece(filter,breakGroup,group);
  // Blades, arrows and spells find the prop of a remnant by this payload.
  body->setPayload(parent,DynamicWorld::P_Fragment);
  body->setLabel(visual+" fragment");
  body->setNoiseProfile(impactMaterial(material),material,visual+" fragment");
  body->setBuoyancyFraction(vobBuoyancyFraction(material));
  return body;
  }

void launchFragment(CollisionWorld::DynamicBody& fragment, const std::vector<uint32_t>& members, const FractureMotion& motion,
                    const Tempest::Vec3& center, const Tempest::Vec3& velocity, const Tempest::Vec3& omega,
                    const Tempest::Vec3& point, const Tempest::Vec3& direction, size_t version) {
  const auto hit=direction.quadLength()>1.e-12f ? Tempest::Vec3::normalize(direction) : Tempest::Vec3();
  const auto r=(fragment.centerOfMass()-center)*.01f;
  const Tempest::Vec3 tangential(omega.y*r.z-omega.z*r.y,omega.z*r.x-omega.x*r.z,omega.x*r.y-omega.y*r.x);
  if(members.size()>1) {
    // A joined group keeps the inherited motion and takes the blow by its own centre of
    // mass: no push from the prop's centre and no added spin.
    const float distance=(fragment.centerOfMass()-point).length();
    auto added=hit*(motion.hit*std::exp(-distance/motion.falloff));
    const float speed=added.length();
    if(speed>motion.cap) added*=motion.cap/speed;
    fragment.setVelocity(velocity+tangential*100.f+added,omega);
    return;
    }
  const size_t i=members.front();
  // A small separation velocity releases touching cut faces. Linear and
  // angular momentum inherited from the intact body remain the base motion.
  auto outward=r.quadLength()>1.e-6f ? Tempest::Vec3::normalize(r) : Tempest::Vec3();
  auto spin=Tempest::Vec3();
  if(version==3) {
    const float distance=(fragment.centerOfMass()-point).length();
    outward=outward*motion.outward+hit*(motion.hit*std::exp(-distance/motion.falloff));
    const float speed=outward.length();
    if(speed>motion.cap) outward*=motion.cap/speed;
    const Tempest::Vec3 axis(hit.y*r.z-hit.z*r.y,hit.z*r.x-hit.x*r.z,hit.x*r.y-hit.y*r.x);
    if(axis.quadLength()>1.e-12f)
      spin=Tempest::Vec3::normalize(axis)*(motion.spin*float(i%8+1)/8.f);
    }
  else outward*=40.f;
  fragment.setVelocity(velocity+tangential*100.f+outward,omega+spin);
  }
}

void DynamicWorld::breakMovableVob(BodyData& obj) {
  if(!obj.pendingBreak || obj.body==nullptr) return;
  const auto started=std::chrono::steady_clock::now();
  const auto matrix=obj.body->matrix();
  const auto velocity=obj.body->velocity(),omega=obj.body->angularVelocity();
  const auto center=obj.body->centerOfMass();
  const auto& mesh=*obj.movableShape->fracture;
  // Bonds of format 3 decide which pieces stay joined: the blow point in the visual's frame.
  std::vector<uint32_t> pieceGroup(mesh.pieces.size());
  for(uint32_t i=0;i<pieceGroup.size();++i) pieceGroup[i]=i;
  std::vector<bool> broken(mesh.bonds.size(),true);
  if(!mesh.bonds.empty()) {
    auto inverse=matrix;
    inverse.inverse();
    auto local=obj.breakPoint;
    inverse.project(local);
    pieceGroup=fractureGroups(mesh,movableVobs->fractureBonds,obj.breakHit,local,&broken);
    }
  const auto members=fractureMembers(pieceGroup);
  std::vector<std::unique_ptr<CollisionWorld::DynamicBody>> fragments;
  fragments.reserve(members.size());
  const uint32_t breakGroup=mesh.version==3 ? world->newFractureGroup() : 0;
  const auto breakFilter=mesh.version==3 ? fractureGroupFilter(mesh,pieceGroup,uint32_t(members.size())) : nullptr;
  // All hulls, triangle data and GPU meshes are prepared while loading the
  // visual. The replacement only installs native bodies and transfers motion.
  for(uint32_t g=0;g<members.size();++g) {
    auto body=addFragmentBody(*world,mesh,members[g],matrix,obj.movableMass,obj.movableShape->material,
                              obj.movableShape->friction,obj.visual,breakFilter.GetPtr(),breakGroup,g,&obj);
    launchFragment(*body,members[g],movableVobs->fractureMotion,center,velocity,omega,obj.breakPoint,obj.breakDirection,mesh.version);
    // A character the new body appeared around passes it until they separate.
    world->passCharacterOverlaps(*body);
    fragments.push_back(std::move(body));
    }
  obj.current=matrix;
  obj.pose->transform=matrix;
  obj.pose->broken=true; // Existing pinned arrows invalidate this parent.
  obj.body.reset();
  obj.query.clear();
  obj.fragments=std::move(fragments);
  obj.fragmentGroup=std::move(pieceGroup);
  obj.fragmentBondBroken=std::move(broken);
  obj.fragmentCollisionGroup=breakGroup;
  obj.fragmentSleepTime.assign(obj.fragments.size(),-1);
  obj.fragmentPermanent.assign(obj.fragments.size(),false);
  obj.fragmentRemnant.assign(obj.fragments.size(),{});
  chooseLastingGroups(obj);
  obj.pendingBreak=false;
  obj.trace=obj.tilted=false;
  obj.fragmentsWereSleeping=false;
  syncFragments(obj);
  emitBreakNoise(obj,center);
  if(TestBench::isActive())
    TestBench::destructionBreak(reinterpret_cast<uintptr_t>(&obj),obj.fragmentGroup.size(),
                               float(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count()));
  }

void DynamicWorld::chooseLastingGroups(BodyData& obj) {
  // Keep stable piece and group indices throughout cleanup and save/load. The three largest
  // present groups by volume stay; equal volumes are ordered by group index (by smallest piece).
  // A group that drops out of the three starts the usual cleanup time from now.
  const auto& mesh=*obj.movableShape->fracture;
  std::vector<double> groupVolume(obj.fragments.size(),0.);
  for(size_t i=0;i<mesh.pieces.size() && i<obj.fragmentGroup.size();++i)
    if(obj.fragmentGroup[i]<groupVolume.size()) groupVolume[obj.fragmentGroup[i]]+=mesh.pieces[i].volume;
  std::vector<size_t> largest;
  for(size_t g=0;g<obj.fragments.size();++g)
    if(obj.fragments[g]!=nullptr) largest.push_back(g);
  std::stable_sort(largest.begin(),largest.end(),[&](size_t a,size_t b) {
    return groupVolume[a]>groupVolume[b];
    });
  std::vector<bool> lasting(obj.fragments.size(),false);
  for(size_t i=0;i<std::min(size_t(3),largest.size());++i)
    lasting[largest[i]]=true;
  for(size_t g=0;g<obj.fragments.size();++g) {
    if(obj.fragmentPermanent[g] && !lasting[g]) obj.fragmentSleepTime[g]=-1;
    obj.fragmentPermanent[g]=lasting[g];
    }
  }

void DynamicWorld::emitBreakNoise(const BodyData& obj, const Tempest::Vec3& center) {
  PhysicsNoise noise;
  noise.position=center;
  // Some wooden Gothic barrels label their source mesh as stone. Keep that
  // authored density/friction for physics, but use the destructible class for
  // the sound of the newly exposed material.
  const bool wooden=obj.fractureRule->className=="crate" || obj.fractureRule->className=="barrel" ||
                    obj.fractureRule->className=="fence";
  noise.material=wooden ? MAT_WOOD : MAT_CLAY;
  noise.surface=wooden ? asset::MaterialGroup::WOOD : asset::MaterialGroup::STONE;
  noise.what=obj.visual+" break";
  noise.impulse=obj.breakImpulse;
  noise.volume=1.f;
  noise.radius=1600.f;
  noise.body=reinterpret_cast<uintptr_t>(&obj);
  world->emitPhysicsNoise(noise);
  }

void DynamicWorld::syncFragments(BodyData& obj) {
  if(obj.fragments.empty()) return;
  const bool sleeping=std::all_of(obj.fragments.begin(),obj.fragments.end(),[](const auto& body) {
    return body==nullptr || body->isSleeping();
    });
  if(sleeping && obj.fragmentsWereSleeping) return;
  obj.fragmentsWereSleeping=sleeping;
  if(!obj.fractureCallback) return;
  std::vector<Tempest::Matrix4x4> matrices;
  matrices.reserve(obj.fragmentGroup.size());
  // One matrix per piece: the body of its bond group.
  for(const auto group:obj.fragmentGroup) {
    const auto& body=obj.fragments[group];
    if(body!=nullptr) {
      matrices.push_back(body->matrix());
      }
    else {
      // Preserve the mesh batch-to-piece mapping while hiding retired pieces.
      auto hidden=Tempest::Matrix4x4::mkIdentity();
      hidden.translate(0,-10000000.f,0);
      matrices.push_back(hidden);
      }
    }
  obj.fractureCallback(true,matrices);
  }

void DynamicWorld::cleanupFragments(BodyData& obj) {
  bool changed=false;
  for(size_t i=0;i<obj.fragments.size();++i) {
    auto& body=obj.fragments[i];
    if(body==nullptr)
      continue;
    auto& since=obj.fragmentSleepTime[i];
    if(!body->isSleeping()) {
      since=-1;
      continue;
      }
    if(traceTime>=0 && (since<0 || traceTime<since))
      since=traceTime;
    const bool expired=since>=0 && traceTime>=since && traceTime-since>=30;
    const bool distant=hasFragmentObserver && (body->centerOfMass()-fragmentObserver).quadLength()>3000.f*3000.f;
    if(!obj.fragmentPermanent[i] && (expired || distant)) {
      // A group retires whole: every piece of it.
      if(TestBench::isActive())
        for(size_t piece=0;piece<obj.fragmentGroup.size();++piece)
          if(obj.fragmentGroup[piece]==i)
            TestBench::destructionRetire(reinterpret_cast<uintptr_t>(&obj),piece,expired,
                                         hasFragmentObserver ? (body->centerOfMass()-fragmentObserver).length() : -1.f);
      body.reset();
      changed=true;
      }
    }
  if(changed)
    obj.fragmentsWereSleeping=false;
  }

void DynamicWorld::saveDestructionVobs(Serialize& fout) const {
  if(!movableVobs->destructionEnabled)
    return;
  std::vector<const BodyData*> changed;
  for(const auto* obj:movableVobs->bodies)
    if(obj->pose->zenId!=uint32_t(-1) && obj->fractureRule!=nullptr && obj->movableShape->fracture!=nullptr &&
       (obj->pose->broken || obj->health<obj->fractureRule->health))
      changed.push_back(obj);
  if(changed.empty())
    return;
  // Separate optional archive entry: older saves have no destruction state,
  // and movable-vobs readers keep their existing schema and meaning.
  fout.setEntry("worlds/",fout.worldName(),"/destruction-vobs");
  // Schema 3: a pending break keeps its blow, each piece its bond group, and the
  // native state is per group (reports/physics/2026-11-destruction-bonds). Schema 4
  // adds the fate of every bond and, per group, a remnant's damage and pending blow.
  fout.write(uint32_t(0x31534544),uint16_t(4),fout.worldName(),uint32_t(changed.size()));
  for(const auto* obj:changed) {
    fout.write(obj->pose->zenId,obj->visual,obj->origin,obj->current,obj->health,obj->pose->broken,
               uint32_t(obj->fragmentGroup.size()),uint64_t(obj->traceTime)+1,obj->traceCulprit);
    fout.write(obj->pendingBreak);
    if(obj->pendingBreak)
      fout.write(uint8_t(obj->breakHit),obj->breakPoint,obj->breakDirection,obj->breakImpulse);
    for(const auto group:obj->fragmentGroup)
      fout.write(group);
    fout.write(uint32_t(obj->fragments.size()));
    for(size_t i=0;i<obj->fragments.size();++i) {
      const auto& body=obj->fragments[i];
      fout.write(body!=nullptr,obj->fragmentPermanent[i],uint64_t(obj->fragmentSleepTime[i])+1);
      if(body!=nullptr)
        fout.write(body->matrix(),body->velocity(),body->angularVelocity(),body->isSleeping());
      }
    fout.write(uint32_t(obj->fragmentBondBroken.size()));
    for(const bool broken:obj->fragmentBondBroken)
      fout.write(broken);
    for(size_t i=0;i<obj->fragments.size();++i) {
      const auto remnant=i<obj->fragmentRemnant.size() ? obj->fragmentRemnant[i] : BodyData::Remnant{};
      fout.write(remnant.damage,remnant.pending);
      if(remnant.pending)
        fout.write(uint8_t(remnant.hit),remnant.point,remnant.direction,remnant.impulse);
      }
    }
  }

void DynamicWorld::loadDestructionVobs(Serialize& fin) {
  if(!movableVobs->destructionEnabled || !fin.setEntry("worlds/",fin.worldName(),"/destruction-vobs"))
    return;
  try {
    uint32_t magic=0,count=0;
    uint16_t schema=0;
    std::string worldName;
    fin.read(magic,schema,worldName,count);
    if(magic!=0x31534544 || schema<1 || schema>4 || worldName!=fin.worldName() || count>100000)
      return;
    struct Piece {
      bool present=false,permanent=false,sleeping=true;
      uint64_t sleepTime=0;
      Tempest::Matrix4x4 matrix;
      Tempest::Vec3 velocity,omega;
      };
    const auto finiteVelocity=[](const Tempest::Vec3& v) {
      return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && v.quadLength()<1.e14f;
      };
    for(uint32_t record=0;record<count;++record) {
      uint32_t id=uint32_t(-1),pieceCount=0;
      std::string visual;
      Tempest::Matrix4x4 origin,matrix;
      float health=0;
      bool broken=false;
      fin.read(id,visual,origin,matrix,health,broken,pieceCount);
      uint64_t traceTimeEncoded=0;
      std::string traceCulprit;
      if(schema>=2)
        fin.read(traceTimeEncoded,traceCulprit);
      bool pending=false;
      uint8_t pendingHit=0;
      Tempest::Vec3 pendingPoint,pendingDirection;
      float pendingImpulse=0;
      if(schema>=3) {
        fin.read(pending);
        if(pending)
          fin.read(pendingHit,pendingPoint,pendingDirection,pendingImpulse);
        }
      if(pieceCount>64)
        return;
      // Older schemas have no bond groups: each piece is its own group.
      std::vector<uint32_t> pieceGroup(pieceCount);
      uint32_t groupCount=pieceCount;
      for(uint32_t i=0;i<pieceCount;++i) pieceGroup[i]=i;
      if(schema>=3) {
        for(auto& group:pieceGroup) fin.read(group);
        fin.read(groupCount);
        if(groupCount>pieceCount)
          return;
        }
      std::vector<Piece> pieces(groupCount);
      bool valid=std::isfinite(health) && health>=0.f && PhysicsSave::validTransform(matrix) &&
                 PhysicsSave::sameScale(matrix,origin) && (broken || pieceCount==0) &&
                 (!pending || (!broken && health<=.001f && pendingHit<=uint8_t(FractureHit::Spell) &&
                               finiteVelocity(pendingPoint) && finiteVelocity(pendingDirection) &&
                               std::isfinite(pendingImpulse)));
      std::vector<bool> used(groupCount,false);
      for(const auto group:pieceGroup) {
        valid &= group<groupCount;
        if(group<groupCount) used[group]=true;
        }
      valid &= std::all_of(used.begin(),used.end(),[](bool value) { return value; });
      size_t permanentCount=0;
      for(auto& piece:pieces) {
        fin.read(piece.present,piece.permanent,piece.sleepTime);
        permanentCount+=piece.permanent;
        valid &= piece.sleepTime<=uint64_t(std::numeric_limits<int64_t>::max())+1;
        valid &= !piece.permanent || piece.present;
        if(piece.present) {
          fin.read(piece.matrix,piece.velocity,piece.omega,piece.sleeping);
          valid &= PhysicsSave::validTransform(piece.matrix) && PhysicsSave::sameScale(piece.matrix,origin) &&
                   finiteVelocity(piece.velocity) && finiteVelocity(piece.omega);
          }
        }
      // Schema 4: each bond's fate, then per group a remnant's damage and pending blow.
      std::vector<bool> savedBonds;
      std::vector<BodyData::Remnant> remnants(groupCount);
      if(schema>=4) {
        uint32_t bondCount=0;
        fin.read(bondCount);
        if(bondCount>1024)
          return;
        savedBonds.resize(bondCount);
        for(uint32_t k=0;k<bondCount;++k) {
          bool cut=false;
          fin.read(cut);
          savedBonds[k]=cut;
          }
        for(auto& remnant:remnants) {
          fin.read(remnant.damage,remnant.pending);
          valid &= std::isfinite(remnant.damage) && remnant.damage>=0.f;
          if(remnant.pending) {
            uint8_t hit=0;
            fin.read(hit,remnant.point,remnant.direction,remnant.impulse);
            remnant.hit=FractureHit(hit);
            valid &= hit<=uint8_t(FractureHit::Spell) && finiteVelocity(remnant.point) &&
                     finiteVelocity(remnant.direction) && std::isfinite(remnant.impulse);
            }
          }
        }
      if(!valid || (broken && permanentCount!=std::min(size_t(3),pieces.size())))
        continue;
      for(auto* obj:movableVobs->bodies) {
        if(id==uint32_t(-1) || obj->pose->zenId!=id || obj->visual!=visual ||
           !PhysicsSave::sameTransform(obj->origin,origin) || obj->fractureRule==nullptr ||
           obj->movableShape->fracture==nullptr || health>obj->fractureRule->health)
          continue;
        if(!broken) {
          obj->health=health;
          // A break struck just before saving happens on the next tick, with its blow.
          if(pending && obj->body!=nullptr) {
            obj->pendingBreak=true;
            obj->breakHit=FractureHit(pendingHit);
            obj->breakPoint=pendingPoint;
            obj->breakDirection=pendingDirection;
            obj->breakImpulse=pendingImpulse;
            obj->traceTime=traceTimeEncoded>0 && traceTimeEncoded-1<=uint64_t(std::numeric_limits<int64_t>::max())
                             ? int64_t(traceTimeEncoded-1) : -1;
            obj->traceCulprit=traceCulprit;
            }
          break;
          }
        const auto& mesh=*obj->movableShape->fracture;
        if(pieceGroup.size()!=mesh.pieces.size())
          break;
        // Loading debris does not enable movement of any intact prop. Its
        // authored hull is needed only to recover the original fragment mass.
        prepareMovableVobShape(*obj);
        if(!std::isfinite(obj->movableMass) || obj->movableMass<=0.f)
          break;
        // Groups of more than one piece need the bonds that made them.
        const auto members=fractureMembers(pieceGroup);
        if(mesh.version!=3 && members.size()!=pieceGroup.size())
          break;
        std::vector<std::unique_ptr<CollisionWorld::DynamicBody>> fragments(pieces.size());
        const uint32_t breakGroup=mesh.version==3 ? world->newFractureGroup() : 0;
        const auto breakFilter=mesh.version==3 ? fractureGroupFilter(mesh,pieceGroup,groupCount) : nullptr;
        for(uint32_t i=0;i<pieces.size();++i) {
          const auto& piece=pieces[i];
          if(!piece.present)
            continue;
          auto body=addFragmentBody(*world,mesh,members[i],piece.matrix,obj->movableMass,obj->movableShape->material,
                                    obj->movableShape->friction,obj->visual,breakFilter.GetPtr(),breakGroup,i,obj);
          body->restoreTransform(piece.matrix,piece.sleeping);
          if(!piece.sleeping)
            body->setVelocity(piece.velocity,piece.omega);
          fragments[i]=std::move(body);
          }
        obj->body.reset();
        obj->query.clear();
        obj->current=matrix;
        obj->pose->transform=matrix;
        obj->pose->broken=true;
        obj->health=0.f;
        obj->pendingBreak=false;
        obj->trace=obj->tilted=false;
        // Step-2 saves have debris but no recorded break time or actor.
        obj->traceTime=traceTimeEncoded>0 && traceTimeEncoded-1<=uint64_t(std::numeric_limits<int64_t>::max())
                         ? int64_t(traceTimeEncoded-1) : -1;
        obj->traceCulprit=std::move(traceCulprit);
        obj->fragments=std::move(fragments);
        // Schema 4 keeps each bond's fate. Older saves (and a save that disagrees with its
        // groups) join the pieces of one group by all their bonds and nothing across groups.
        std::vector<bool> bonds(mesh.bonds.size());
        for(size_t k=0;k<bonds.size();++k) {
          const auto& bond=mesh.bonds[k];
          bonds[k]=pieceGroup[bond.a]!=pieceGroup[bond.b] || (savedBonds.size()==bonds.size() && savedBonds[k]);
          }
        if(fractureComponents(mesh,bonds)!=pieceGroup)
          for(size_t k=0;k<bonds.size();++k)
            bonds[k]=pieceGroup[mesh.bonds[k].a]!=pieceGroup[mesh.bonds[k].b];
        for(uint32_t g=0;g<remnants.size();++g)
          if(members[g].size()<2 || obj->fragments[g]==nullptr) remnants[g]={};
        obj->fragmentBondBroken=std::move(bonds);
        obj->fragmentRemnant=std::move(remnants);
        obj->fragmentCollisionGroup=breakGroup;
        obj->fragmentGroup=std::move(pieceGroup);
        obj->fragmentSleepTime.resize(pieces.size());
        obj->fragmentPermanent.resize(pieces.size());
        for(size_t i=0;i<pieces.size();++i) {
          obj->fragmentSleepTime[i]=pieces[i].sleepTime==0 ? -1 : int64_t(pieces[i].sleepTime-1);
          obj->fragmentPermanent[i]=pieces[i].permanent;
          }
        obj->fragmentsWereSleeping=false;
        syncFragments(*obj);
        break;
        }
      }
    }
  catch(const std::exception& e) {
    Tempest::Log::e("destruction vob load: ",e.what());
    }
  }

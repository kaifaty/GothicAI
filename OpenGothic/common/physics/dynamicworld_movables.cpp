#include "dynamicworld_internal.h"

#include "physicmeshshape.h"
#include "cloth.h"
#include "graphics/mesh/skeleton.h"
#include "graphics/mesh/protomesh.h"
#include "savearchive.h"
#include "game/physicssave.h"
#include "testbench.h"

#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/OffsetCenterOfMassShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>

#include <cctype>
#include <chrono>
#include <map>
#include <stdexcept>
#include <unordered_set>

float DynamicWorldInternal::vobBuoyancyFraction(asset::MaterialGroup material, std::string_view className) {
  if(material!=asset::MaterialGroup::WOOD)
    return DynamicWorld::materialDensity(material)/1000.f;
  // Water displacement is independent of native push mass and its CSV multipliers.
  // Classless wood, including broken pieces, uses the solid-wood target.
  if(className=="barrel") return 1.f/3.f;
  if(className=="crate") return .5f;
  if(className=="table" || className=="cupboard") return .9f;
  return .6f;
  }

namespace {
bool noticeableMovableTrace(const Tempest::Matrix4x4& origin, const Tempest::Matrix4x4& current, bool& tilted) {
  double originLength=0.,currentLength=0.,dot=0.;
  for(int component=0;component<3;++component) {
    const double a=origin.at(1,component),b=current.at(1,component);
    originLength+=a*a;
    currentLength+=b*b;
    dot+=a*b;
    }
  // Compare the original and current local up axes, so rotating around the
  // upright axis cannot turn a table in place into a fallen-table trace.
  // Float matrices and the native quaternion roundtrip can put exactly 30
  // degrees a few ulps over the boundary.
  tilted = originLength>0. && currentLength>0. && dot/std::sqrt(originLength*currentLength)<0.8660254037844386-1.e-6;
  double displacement=0.;
  for(int component=0;component<3;++component) {
    const double delta=double(current.at(3,component))-origin.at(3,component);
    displacement+=delta*delta;
    }
  return displacement>50.*50. || tilted;
  }

}

DynamicWorld::Item DynamicWorld::addMovableVob(const ProtoMesh& proto, std::string_view visual,
                                              const Tempest::Matrix4x4& matrix, float massMultiplier,
                                              bool blocksWalking, std::string_view className,
                                              bool legacyCollision, uint32_t zenId) {
  if(!std::isfinite(massMultiplier) || massMultiplier<=0.f)
    return {};
  std::string key(visual);
  std::transform(key.begin(),key.end(),key.begin(),[](unsigned char c) { return char(std::toupper(c)); });
  auto [cached,inserted] = movableVobs->shapes.try_emplace(key);
  if(inserted) {
    auto shape = std::make_shared<MovableVobs::Shape>();
    size_t largestAttach = 0;
    for(size_t i=0;i<proto.attach.size();++i) {
      const auto* source = proto.attach[i].shape.get();
      if(source==nullptr || source->mesh.shape()==nullptr)
        continue;
      auto local = Tempest::Matrix4x4::mkIdentity();
      for(size_t node=0;node<proto.nodes.size();++node) {
        if(proto.nodes[node].attachId!=i)
          continue;
        if(proto.skeleton!=nullptr && node<proto.skeleton->tr.size()) {
          local = proto.skeleton->tr[node];
          }
        else {
          local = proto.nodes[node].transform;
          size_t parent = proto.nodes[node].parentId;
          // Nodes normally come from a Skeleton. Bound the fallback traversal
          // so malformed model hierarchy data cannot loop while loading a world.
          for(size_t depth=0;parent<proto.nodes.size() && depth<proto.nodes.size();++depth) {
            auto parentMatrix = proto.nodes[parent].transform;
            parentMatrix.mul(local);
            local = parentMatrix;
            parent = proto.nodes[parent].parentId;
            }
          }
        break;
        }
      const auto size = source->mesh.vertexCount();
      if(size>largestAttach) {
        largestAttach = size;
        shape->material = source->material;
        }
      shape->friction += source->friction();
      shape->parts.push_back({source,local});
      }
    if(!shape->parts.empty()) {
      shape->friction /= float(shape->parts.size());
      if(FractureRule::classify(visual)!=nullptr) {
        shape->fracture=FractureMesh::load(visual);
        if(shape->fracture==nullptr) {
          static bool warned=false;
          if(!warned) {
            warned=true;
            Tempest::Log::i("Destruction cache unavailable; uncached visuals keep their original physics (OPENGOTHIC_FRACTURE)");
            }
          }
        }
      cached->second = std::move(shape);
      }
    }
  if(cached->second==nullptr)
    return {};
  auto obj = std::make_unique<BodyData>();
  obj->movableShape = cached->second;
  obj->origin = obj->current = matrix;
  obj->visual = std::string(visual);
  obj->pose->zenId = zenId;
  obj->pose->visual = obj->visual;
  obj->pose->origin = matrix;
  obj->pose->transform = matrix;
  obj->className = std::string(className);
  obj->blocksWalking = blocksWalking;
  obj->legacyCollision = legacyCollision;
  obj->massMultiplier = massMultiplier;
  obj->fractureRule=FractureRule::classify(visual);
  obj->localBounds[0] = proto.bbox()[0];
  obj->localBounds[1] = proto.bbox()[1];
  rebuildMovableVob(*obj);
  if((movableVobs->enabled || className=="fence") && obj->body==nullptr)
    return {};
  obj->movableOwner = movableVobs.get();
  movableVobs->bodies.push_back(obj.get());
  return Item(this,obj.release());
  }

void DynamicWorld::prepareMovableVobShape(BodyData& obj) {
  auto& shape = *obj.movableShape;
  if(!shape.hullAttempted) {
    shape.hullAttempted = true;
    std::vector<JPH::Vec3> points;
    for(const auto& part:shape.parts)
      part.source->mesh.appendVertices(points,part.local);
    if(points.size()>=4) {
      Physics::initialize();
      // Preserve this envelope for mass, water and fracture, even when native
      // collision uses the offline compound. Mesh queries keep the source faces.
      JPH::ConvexHullShapeSettings settings(points.data(),int(points.size()),0.001f);
      const auto result = settings.Create();
      if(!result.HasError() && result.Get()->GetVolume()>0.f)
        shape.hull = result.Get();
      }
    if(shape.hull==nullptr)
      Tempest::Log::e("Cannot build movable vob hull: ",obj.visual);
    }
  if(shape.hull!=nullptr)
    obj.movableMass = float(shape.hull->GetVolume()*volumeScale(obj.origin)*materialDensity(shape.material)*obj.massMultiplier);
  if((obj.className=="table" || obj.className=="cupboard") && movableVobs->furniturePartsEnabled && !shape.convexPartsAttempted) {
    shape.convexPartsAttempted = true;
    shape.convexParts = ConvexParts::load(obj.visual);
    }
  }

void DynamicWorld::rebuildMovableVob(BodyData& obj) {
  obj.body.reset();
  obj.query.clear();
  obj.fragments.clear();
  obj.fragmentSleepTime.clear();
  obj.fragmentPermanent.clear();
  obj.fragmentGroup.clear();
  obj.fragmentBondBroken.clear();
  obj.fragmentRemnant.clear();
  obj.pendingBreak=false;
  obj.pose->broken=false;
  obj.health=obj.fractureRule!=nullptr ? obj.fractureRule->health : 0.f;
  obj.current = obj.origin;
  obj.wasSleeping = true;
  obj.trace = obj.tilted = false;
  obj.traceTime = -1;
  obj.traceCulprit.clear();
  auto& shape = *obj.movableShape;
  const bool fence=obj.className=="fence";
  if(movableVobs->enabled || fence)
    prepareMovableVobShape(obj);
  const bool allowed = obj.disposition.empty() || obj.disposition=="ok" || obj.disposition=="item";
  const bool enabled = (movableVobs->enabled || fence) && (allowed || fence) && shape.hull!=nullptr &&
                       std::isfinite(obj.movableMass) && obj.movableMass>0.f;
  if(enabled) {
    if(fence)
      // The authored mesh remains the exact static obstacle. This sensor hull
      // only owns destruction identity and the mass used by its future pieces.
      obj.body=world->addCollisionBody(shape.hull,obj.origin,shape.friction,C_MovableVob,false,true);
    else {
      const bool compound = (obj.className=="table" || obj.className=="cupboard") && movableVobs->furniturePartsEnabled &&
                            shape.convexParts!=nullptr && shape.convexParts->partCount>1;
      const auto& collision = compound ? shape.convexParts->shape : shape.hull;
      obj.body = world->addDynamicBody(collision,obj.origin,shape.friction,obj.movableMass,C_MovableVob,true);
      if(compound)
        obj.body->setBuoyancyShape(new JPH::OffsetCenterOfMassShape(shape.hull,
          collision->GetCenterOfMass()-shape.hull->GetCenterOfMass()));
      }
    }
  if(obj.body!=nullptr) {
    obj.body->setPayload(&obj,P_MovableVob);
    obj.body->setLabel(obj.visual);
    obj.body->setNoiseProfile(impactMaterial(shape.material),shape.material,obj.visual);
    obj.body->setBuoyancyFraction(vobBuoyancyFraction(shape.material,obj.className));
    }
  // cd_dynamic controls the original mesh queries independently of simulation.
  // The hull can move and receive impacts without becoming a new walking/ray obstacle.
  if(obj.legacyCollision) {
    const int category = !enabled || fence ? C_Object : obj.blocksWalking ? C_MovableQuery : C_SmallVobQuery;
    for(const auto& part:shape.parts) {
      auto matrix = obj.origin;
      matrix.mul(part.local);
      obj.query.push_back(world->addCollisionBody(part.source->mesh.shape(),matrix,part.source->friction(),category,
                                                 enabled && !fence,enabled && !fence,&part.source->mesh));
      obj.query.back()->setPayload(&obj,P_MovableVob);
      obj.query.back()->setLabel(obj.visual);
      }
    }
  obj.pose->transform = obj.current;
  if(obj.transformCallback)
    obj.transformCallback(obj.current);
  if(obj.fractureCallback) obj.fractureCallback(false,{});
  }

void DynamicWorld::syncMovableVob(BodyData& obj) {
  if(obj.body==nullptr)
    return;
  const bool sleeping = obj.body->isSleeping();
  if(sleeping && obj.wasSleeping)
    return;
  obj.wasSleeping = sleeping;
  obj.current = obj.body->matrix();
  obj.pose->transform = obj.current;
  const bool trace = noticeableMovableTrace(obj.origin,obj.current,obj.tilted);
  if(trace && !obj.trace) {
    obj.traceTime = traceTime;
    obj.traceCulprit = obj.body->traceCulprit();
    }
  else if(!trace) {
    obj.traceTime = -1;
    obj.traceCulprit.clear();
    }
  obj.trace = trace;
  for(size_t i=0;i<obj.query.size();++i) {
    auto matrix = obj.current;
    matrix.mul(obj.movableShape->parts[i].local);
    obj.query[i]->setObjMatrix(matrix);
    }
  if(obj.transformCallback)
    obj.transformCallback(obj.current);
  }

void DynamicWorld::setMovableVobsEnabled(bool enabled) {
  if(movableVobs->enabled==enabled)
    return;
  movableVobs->enabled = enabled;
  for(auto* obj:movableVobs->bodies)
    if(obj->className!="fence" && !obj->pose->broken)
      rebuildMovableVob(*obj);
  if(enabled && movableVobs->finalized && movableVobs->census.empty())
    validateMovableVobs();
  }

void DynamicWorld::setFurniturePartsEnabled(bool enabled) {
  if(movableVobs->furniturePartsEnabled==enabled)
    return;
  movableVobs->furniturePartsEnabled = enabled;
  for(auto* obj:movableVobs->bodies) {
    if((obj->className!="table" && obj->className!="cupboard") || obj->pose->broken || obj->body==nullptr)
      continue;
    prepareMovableVobShape(*obj);
    const auto& shape = *obj->movableShape;
    const bool compound = enabled && shape.convexParts!=nullptr && shape.convexParts->partCount>1;
    const auto& collision = compound ? shape.convexParts->shape : shape.hull;
    const auto matrix = obj->body->matrix();
    const auto velocity = obj->body->velocity();
    const auto angular = obj->body->angularVelocity();
    const bool sleeping = obj->body->isSleeping();
    obj->body.reset();
    obj->body = world->addDynamicBody(collision,matrix,shape.friction,obj->movableMass,C_MovableVob,sleeping);
    obj->body->setPayload(obj,P_MovableVob);
    obj->body->setLabel(obj->visual);
    obj->body->setNoiseProfile(impactMaterial(shape.material),shape.material,obj->visual);
    obj->body->setBuoyancyFraction(vobBuoyancyFraction(shape.material,obj->className));
    if(compound)
      obj->body->setBuoyancyShape(new JPH::OffsetCenterOfMassShape(shape.hull,
        collision->GetCenterOfMass()-shape.hull->GetCenterOfMass()));
    if(!sleeping)
      obj->body->setVelocity(velocity,angular);
    }
  }

bool DynamicWorld::furniturePartsEnabled() const {
  return movableVobs->furniturePartsEnabled;
  }

void DynamicWorld::setDestructionEnabled(bool enabled) {
  if(movableVobs->destructionEnabled==enabled) return;
  movableVobs->destructionEnabled=enabled;
  for(auto* obj:movableVobs->bodies) {
    obj->pendingBreak=false;
    obj->health=obj->fractureRule!=nullptr ? obj->fractureRule->health : 0.f;
    if(!enabled && obj->pose->broken) rebuildMovableVob(*obj);
    }
  }

bool DynamicWorld::destructionEnabled() const {
  return movableVobs->destructionEnabled;
  }

void DynamicWorld::finalizeMovableVobs() {
  movableVobs->finalized = true;
  // Capture the authored geometry even when starting with physics disabled:
  // a later enable must not classify against a moved door or a loaded save.
  const bool enabled = movableVobs->enabled;
  if(!enabled) {
    movableVobs->enabled = true;
    for(auto* obj:movableVobs->bodies)
      if(obj->className!="fence" && !obj->pose->broken)
        rebuildMovableVob(*obj);
    }
  validateMovableVobs();
  if(!enabled) {
    movableVobs->enabled = false;
    for(auto* obj:movableVobs->bodies)
      if(obj->className!="fence" && !obj->pose->broken)
        rebuildMovableVob(*obj);
    }
  }

double DynamicWorld::movableVobValidationMilliseconds() const {
  return movableVobs->validationMs;
  }

void DynamicWorld::validateMovableVobs() {
  const auto start = std::chrono::steady_clock::now();
  // All candidates still have their hulls at ZEN poses here. Decide the entire
  // set before replacing any hull with its original mesh: order cannot affect it.
  auto census = movableVobContacts(1.f);
  std::unordered_set<uint32_t> deepPairs;
  for(const auto& entry:census)
    for(const auto& c:entry.contacts)
      if(c.category==C_MovableVob && c.depth>4.f) {
        deepPairs.insert(entry.zenId);
        if(c.otherZenId!=uint32_t(-1))
          deepPairs.insert(c.otherZenId);
        }
  size_t rejected = 0;
  for(auto& entry:census) {
    const auto onTop = [&](const MovableVobContact& c) {
      return c.category==C_Object && c.side!=0 &&
             (c.otherMin.y-entry.hullMin.y)/std::max(entry.hullMax.y-entry.hullMin.y,1e-3f)>=.9f;
      };
    const auto size = [](const MovableVobContact& c) {
      const auto s = c.otherMax-c.otherMin;
      return std::max({s.x,s.y,s.z});
      };
    const MovableVobContact* deepest = nullptr;
    bool supported = false, carries = false, item = false;
    for(const auto& c:entry.contacts) {
      supported |= c.side==0;
      const bool top = onTop(c);
      const bool small = size(c)<=80.f;
      if(c.depth>4.f && !(top && small) && (deepest==nullptr || c.depth>deepest->depth))
        deepest = &c;
      carries |= top && !small;
      item |= top;
      }
    entry.disposition = !supported ? "hangs" : deepest!=nullptr ?
        (deepest->category==C_MovableVob ? "neighbour" : deepest->side==0 ? "floor" :
         deepest->category==C_Landscape ? "wall" : "structure") :
        carries ? "carries" : deepPairs.count(entry.zenId)!=0 ? "neighbour" : item ? "item" : "ok";
    entry.enabled = entry.disposition=="ok" || entry.disposition=="item";
    rejected += !entry.enabled;
    }
  // Preserve the exact origin geometry for the census, including rejected hulls.
  movableVobs->census = std::move(census);
  for(auto* obj:movableVobs->bodies)
    for(const auto& entry:movableVobs->census)
      if(obj->pose->zenId==entry.zenId) {
        obj->disposition = entry.disposition;
        if(!entry.enabled)
          rebuildMovableVob(*obj);
        break;
        }
  movableVobs->validationMs = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
  Tempest::Log::i("movable vob geometry: ",movableVobs->census.size()," candidates, ",rejected,
                 " static, ",movableVobs->validationMs," ms");
  }

bool DynamicWorld::movableVobsEnabled() const {
  return movableVobs->enabled;
  }

std::shared_ptr<const DynamicWorld::MovableVobPose> DynamicWorld::movableVobPose(uint32_t zenId) const {
  if(zenId==uint32_t(-1))
    return {};
  for(const auto* obj:movableVobs->bodies)
    if(obj->pose->zenId==zenId)
      return obj->pose;
  return {};
  }

std::shared_ptr<const DynamicWorld::MovableVobPose> DynamicWorld::rayMovableVob(const Tempest::Vec3& from,
                                                                           const Tempest::Vec3& to) const {
  const auto hit = world->rayCast(from,to,visualMask,false);
  if(!hit.hasHit || hit.body==nullptr || hit.body->ptrType()!=P_MovableVob)
    return {};
  return static_cast<BodyData*>(hit.body->payload())->pose;
  }

bool DynamicWorld::meleeEligible(const BodyData& obj) const {
  return obj.body!=nullptr && !obj.pose->broken && !obj.pendingBreak && obj.fractureRule!=nullptr &&
         obj.movableShape->fracture!=nullptr && (movableVobs->enabled || obj.className=="fence");
  }

bool DynamicWorld::meleeStrike(BodyData& obj,const Tempest::Vec3& direction,const Tempest::Vec3& point,
                               float weaponDamage,float strength,FractureRule::Melee kind,Npc* attacker) {
  const float damage=obj.fractureRule->meleeDamage(kind,weaponDamage,strength);
  if(!std::isfinite(damage) || damage<=0.f) return false;
  const float impulse=damage*.02f;
  // Fences keep a static sensor hull, so their rejected physical impulse
  // must still record the actor of the real weapon contact.
  obj.body->setTraceCulprit(attacker);
  obj.body->addImpulseAtPoint(direction*impulse,point,attacker);
  obj.health=std::max(0.f,obj.health-damage);
  if(obj.health>.001f) return true;
  obj.pendingBreak=true;
  obj.traceTime=traceTime;
  obj.traceCulprit=obj.body->traceCulprit();
  obj.breakImpulse=impulse;
  obj.breakPoint=point;
  obj.breakDirection=direction;
  obj.breakHit=FractureRule::hit(kind);
  return true;
  }

size_t DynamicWorld::meleeMovableVobs(const Tempest::Vec3& origin,const Tempest::Vec3& forward,
                                     float rangeCm,float weaponDamage,float strength,FractureRule::Melee kind,
                                     Npc* attacker,std::vector<uintptr_t>* struck) {
  if(!movableVobs->destructionEnabled || !std::isfinite(rangeCm) || rangeCm<=0.f ||
     !std::isfinite(origin.x) || !std::isfinite(origin.y) || !std::isfinite(origin.z) ||
     !std::isfinite(forward.x) || !std::isfinite(forward.z)) return 0;
  const auto facing=Tempest::Vec3(forward.x,0,forward.z);
  if(facing.quadLength()<1.e-6f) return 0;
  const float heading=std::atan2(facing.z,facing.x);
  const int arc=kind==FractureRule::Melee::Fist ? 20 : 60;
  std::unordered_set<BodyData*> touched;
  size_t count=0;
  // Test the swept striking arc against the source collision meshes, including
  // downstrokes at low crates. Every ray stops at its first wall or object;
  // range and facing alone never count as a contact. NPC damage keeps its own
  // existing target/defence calculation and consumes exactly the same RNG.
  for(int yaw=-arc;yaw<=arc;yaw+=10) for(int pitch=-40;pitch<=20;pitch+=20) {
    const float a=heading+float(yaw)*(float(M_PI)/180.f);
    const float p=float(pitch)*(float(M_PI)/180.f);
    const Tempest::Vec3 direction(std::cos(a)*std::cos(p),std::sin(p),std::sin(a)*std::cos(p));
    const auto hit=world->rayCast(origin,origin+direction*rangeCm,visualMask,false);
    if(!hit.hasHit || hit.body==nullptr || hit.body->ptrType()!=P_MovableVob) continue;
    auto& obj=*static_cast<BodyData*>(hit.body->payload());
    const auto id=reinterpret_cast<uintptr_t>(&obj);
    if(!meleeEligible(obj) || touched.find(&obj)!=touched.end() ||
       (struck!=nullptr && std::find(struck->begin(),struck->end(),id)!=struck->end())) continue;
    if(!meleeStrike(obj,direction,hit.point,weaponDamage,strength,kind,attacker)) continue;
    touched.insert(&obj);
    if(struck!=nullptr) struck->push_back(id);
    ++count;
    }
  return count;
  }

bool DynamicWorld::meleeStoppedMovableVob(const MeleeBladeContact& contact,float weaponDamage,float strength,
                                          FractureRule::Melee kind,Npc* attacker,std::vector<uintptr_t>& struck) {
  if(contact.movableVob==0 && contact.fragment!=0)
    return meleeStoppedRemnant(contact,weaponDamage,strength,kind,attacker,struck);
  if(!movableVobs->destructionEnabled || !contact.world || !contact.stopsSwing || contact.movableVob==0 ||
     !std::isfinite(contact.point.x) || !std::isfinite(contact.point.y) || !std::isfinite(contact.point.z) ||
     std::find(struck.begin(),struck.end(),contact.movableVob)!=struck.end()) return false;
  // The id came from a swept query; it is only trusted while the body still exists.
  BodyData* found=nullptr;
  for(auto* obj:movableVobs->bodies)
    if(reinterpret_cast<uintptr_t>(obj)==contact.movableVob) {found=obj;break;}
  if(found==nullptr || !meleeEligible(*found)) return false;
  // The blade's own travel at the contact pushes the board; a contact
  // without travel pushes into the struck face.
  auto direction=contact.velocityCmS;
  if(!(direction.quadLength()>1.e-6f)) direction=contact.normal*-1.f;
  if(!(direction.quadLength()>1.e-6f)) return false;
  direction=Tempest::Vec3::normalize(direction);
  if(!meleeStrike(*found,direction,contact.point,weaponDamage,strength,kind,attacker)) return false;
  struck.push_back(contact.movableVob);
  return true;
  }

void DynamicWorld::saveMovableVobs(SaveArchive& fout) const {
  saveDestructionVobs(fout);
  if(!movableVobs->enabled)
    return;
  std::vector<const BodyData*> moved;
  for(const auto* obj:movableVobs->bodies)
    if(obj->pose->zenId!=uint32_t(-1) && obj->body!=nullptr &&
       !PhysicsSave::sameTransform(obj->origin,obj->body->matrix()))
      moved.push_back(obj);
  if(moved.empty())
    return;
  fout.setEntry("worlds/",fout.worldName(),"/movable-vobs");
  fout.write(uint32_t(0x31564f4d),uint16_t(2),fout.worldName(),uint32_t(moved.size()));
  for(const auto* obj:moved)
    fout.write(obj->pose->zenId,obj->visual,obj->origin,obj->body->matrix(),obj->body->isSleeping(),
               obj->trace,uint64_t(obj->traceTime)+1,obj->traceCulprit);
  }

void DynamicWorld::loadMovableVobs(SaveArchive& fin) {
  loadDestructionVobs(fin);
  if(!movableVobs->enabled || !fin.setEntry("worlds/",fin.worldName(),"/movable-vobs"))
    return;
  try {
    uint32_t magic=0,count=0;
    uint16_t schema=0;
    std::string worldName;
    fin.read(magic,schema,worldName,count);
    if(magic!=0x31564f4d || (schema!=1 && schema!=2) || worldName!=fin.worldName() || count>100000)
      return;
    for(uint32_t i=0;i<count;++i) {
      uint32_t id=uint32_t(-1);
      std::string visual;
      Tempest::Matrix4x4 origin,matrix;
      bool sleeping=true;
      fin.read(id,visual,origin,matrix,sleeping);
      bool trace=false;
      uint64_t traceTimeEncoded=0;
      std::string traceCulprit;
      if(schema>=2)
        fin.read(trace,traceTimeEncoded,traceCulprit);
      if(!PhysicsSave::validTransform(matrix) || !PhysicsSave::sameScale(matrix,origin))
        continue;
      for(auto* obj:movableVobs->bodies) {
        if(obj->pose->zenId!=id || id==uint32_t(-1) || obj->visual!=visual ||
           !PhysicsSave::sameTransform(obj->origin,origin))
          continue;
        if(obj->body==nullptr) {
          if(!PhysicsSave::sameTransform(origin,matrix))
            Tempest::Log::i("movable vob save pose ignored: ",id," ",visual," static (",obj->disposition,")");
          break;
          }
        obj->body->restoreTransform(matrix,sleeping);
        // Legacy poses have no recorded appearance time. Mark an existing
        // trace before synchronizing so loading cannot invent a new timestamp.
        obj->trace = schema==1 ? noticeableMovableTrace(origin,matrix,obj->tilted) : trace;
        obj->traceTime = traceTimeEncoded>0 && traceTimeEncoded-1<=uint64_t(std::numeric_limits<int64_t>::max())
                           ? int64_t(traceTimeEncoded-1) : -1;
        obj->traceCulprit = std::move(traceCulprit);
        // Force query meshes, render callback and attachment pose to synchronize
        // even when the restored body is asleep and will never take a step.
        obj->wasSleeping = false;
        syncMovableVob(*obj);
        break;
        }
      }
    }
  catch(const std::exception& e) {
    Tempest::Log::e("movable vob load: ",e.what());
    }
  }

void DynamicWorld::setTraceTime(int64_t minutes) {
  traceTime = std::max(int64_t(-1),minutes);
  }

void DynamicWorld::setFragmentObserver(const Tempest::Vec3& position) {
  clothWorld->setObserver(position);
  fragmentObserver=position;
  hasFragmentObserver=true;
  }

bool DynamicWorld::setMovableVobTransform(uint32_t zenId, const Tempest::Matrix4x4& matrix, Npc* culprit) {
  if(!movableVobs->enabled || zenId==uint32_t(-1) || !PhysicsSave::validTransform(matrix))
    return false;
  for(auto* obj:movableVobs->bodies) {
    if(obj->pose->zenId!=zenId || obj->body==nullptr || !PhysicsSave::sameScale(matrix,obj->origin))
      continue;
    obj->body->setTraceCulprit(culprit);
    obj->body->restoreTransform(matrix,true);
    obj->wasSleeping = false;
    syncMovableVob(*obj);
    return true;
    }
  return false;
  }

std::vector<DynamicWorld::MovableVobContacts> DynamicWorld::movableVobContacts(float marginCm) const {
  if(marginCm==1.f && !movableVobs->census.empty()) {
    auto result = movableVobs->census;
    for(auto& entry:result)
      for(const auto* obj:movableVobs->bodies)
        if(obj->pose->zenId==entry.zenId) {
          entry.enabled = obj->body!=nullptr;
          break;
          }
    return result;
    }
  static const char* materials[] = {"undef","metal","stone","wood","earth","water","snow","none"};
  constexpr uint32_t mask = (1u<<C_Landscape)|(1u<<C_Object)|(1u<<C_MovableVob);
  std::vector<MovableVobContacts> result;
  for(const auto* obj:movableVobs->bodies) {
    if(obj->body==nullptr || obj->className=="fence")
      continue;
    MovableVobContacts entry;
    entry.zenId = obj->pose->zenId;
    entry.visual = obj->visual;
    entry.className = obj->className;
    entry.disposition = obj->disposition;
    entry.enabled = true;
    entry.mass = obj->movableMass;
    entry.position = obj->body->position();
    JPH::Vec3 min, max;
    obj->body->getAabb(min,max);
    entry.hullMin = CollisionWorld::toCentimeters(min);
    entry.hullMax = CollisionWorld::toCentimeters(max);
    // One entry per body and side: the world mesh is a single body holding both floor and walls.
    using Key = std::pair<const CollisionWorld::CollisionBody*,int>;
    std::vector<Key> order;
    std::map<Key,MovableVobContact> byBody;
    const float height = std::max(entry.hullMax.y-entry.hullMin.y,1.f);
    for(const auto& hit:world->overlaps(*obj->body,marginCm,mask)) {
      const bool below = hit.axis.y<=-0.6f && hit.point1.y<=entry.hullMin.y+0.3f*height;
      // side: 0 below, 1 above, 2..5 = +x,-x,+z,-z by the dominant horizontal component
      int side = below ? 0 : hit.axis.y>=0.6f ? 1 :
                 std::abs(hit.axis.x)>=std::abs(hit.axis.z) ? (hit.axis.x>=0 ? 2 : 3) : (hit.axis.z>=0 ? 4 : 5);
      const Key key{hit.body,side};
      auto [it,inserted] = byBody.try_emplace(key);
      auto& c = it->second;
      if(inserted) {
        order.push_back(key);
        c.below = below;
        c.side = side;
        c.category = hit.category;
        c.depth = hit.depth;
        c.pointYMin = c.pointYMax = hit.point1.y;
        if(hit.body!=nullptr) {
          c.interactive = hit.body->ptrType()==P_Interactive;
          c.other = hit.body->label();
          if(hit.body->ptrType()==P_MovableVob) {
            c.other = static_cast<const BodyData*>(hit.body->payload())->visual;
            c.otherZenId = static_cast<const BodyData*>(hit.body->payload())->pose->zenId;
            }
          hit.body->getAabb(min,max);
          c.otherMin = CollisionWorld::toCentimeters(min);
          c.otherMax = CollisionWorld::toCentimeters(max);
          }
        }
      c.count++;
      c.pointYMin = std::min(c.pointYMin,hit.point1.y);
      c.pointYMax = std::max(c.pointYMax,hit.point1.y);
      if(inserted || hit.depth>c.depth) {
        c.depth = hit.depth;
        c.axis = hit.axis;
        c.point = hit.point1;
        c.material = materials[std::min<size_t>(size_t(hit.material),7)];
        c.sector = hit.sector==nullptr ? "" : hit.sector;
        }
      }
    for(const auto& key:order)
      entry.contacts.push_back(byBody[key]);
    result.push_back(std::move(entry));
    }
  return result;
  }

size_t DynamicWorld::wakeMovableVobs() {
  size_t count = 0;
  for(auto* obj:movableVobs->bodies)
    if(obj->body!=nullptr) {
      obj->body->restoreTransform(obj->body->matrix(),false);
      ++count;
      }
  return count;
  }

std::vector<DynamicWorld::MovableVobSnapshot> DynamicWorld::movableVobSnapshot() const {
  std::vector<MovableVobSnapshot> result;
  result.reserve(movableVobs->bodies.size());
  for(auto* obj:movableVobs->bodies) {
    MovableVobSnapshot entry;
    entry.id = reinterpret_cast<uintptr_t>(obj);
    entry.zenId = obj->pose->zenId;
    entry.origin = obj->origin;
    entry.visual = obj->visual;
    entry.className = obj->className;
    entry.blocksWalking = obj->blocksWalking;
    entry.disposition = obj->disposition;
    entry.legacyCollision = obj->legacyCollision;
    entry.enabled = obj->body!=nullptr;
    entry.collisionParts = movableVobs->furniturePartsEnabled && (obj->className=="table" || obj->className=="cupboard") &&
      obj->movableShape->convexParts!=nullptr ? obj->movableShape->convexParts->partCount : 1;
    entry.mass = obj->movableMass;
    entry.hullVolume = obj->movableShape->hull!=nullptr ?
      float(obj->movableShape->hull->GetVolume()*volumeScale(obj->origin)) : 0.f;
    entry.material = obj->movableShape->material;
    entry.density = materialDensity(entry.material);
    entry.massMultiplier = obj->massMultiplier;
    entry.destructible=obj->fractureRule!=nullptr && obj->movableShape->fracture!=nullptr;
    entry.broken=obj->pose->broken;
    entry.health=obj->health;
    entry.maxHealth=obj->fractureRule!=nullptr ? obj->fractureRule->health : 0.f;
    entry.impactThreshold=obj->fractureRule!=nullptr ? obj->fractureRule->threshold*obj->movableMass : 0.f;
    entry.transform = obj->body!=nullptr ? obj->body->matrix() : obj->current;
    entry.localBounds[0] = obj->localBounds[0];
    entry.localBounds[1] = obj->localBounds[1];
    entry.trace = obj->trace;
    entry.tilted = obj->tilted;
    entry.traceTime = obj->traceTime;
    entry.traceCulprit = obj->traceCulprit;
    if(obj->body!=nullptr) {
      entry.velocity = obj->body->velocity();
      entry.sleeping = obj->body->isSleeping();
      entry.touchedByNpc = obj->body->touchedByNpc();
      }
    result.push_back(std::move(entry));
    }
  return result;
  }

std::vector<DynamicWorld::FragmentSnapshot> DynamicWorld::fragmentSnapshot() const {
  std::vector<FragmentSnapshot> result;
  for(const auto* obj:movableVobs->bodies)
    for(size_t i=0;i<obj->fragmentGroup.size();++i) {
      const auto group=obj->fragmentGroup[i];
      if(obj->fragments[group]==nullptr)
        continue;
      const auto& body=*obj->fragments[group];
      result.push_back({reinterpret_cast<uintptr_t>(obj),obj->pose->zenId,i,body.isSleeping(),
                        body.matrix(),body.velocity(),body.angularVelocity(),obj->fragmentPermanent[group],
                        obj->fragmentSleepTime[group],body.friction(),body.centerOfMass(),group,
                        reinterpret_cast<uintptr_t>(&body)});
      }
  return result;
  }

float DynamicWorld::fragmentGroundPenetration(uintptr_t parentId, size_t piece) const {
  for(const auto* obj:movableVobs->bodies) {
    if(reinterpret_cast<uintptr_t>(obj)!=parentId || piece>=obj->fragmentGroup.size() ||
       obj->fragments[obj->fragmentGroup[piece]]==nullptr)
      continue;
    float depth=0;
    for(const auto& contact:world->overlaps(*obj->fragments[obj->fragmentGroup[piece]],0.f,(1u<<C_Landscape)|(1u<<C_Object)))
      depth=std::max(depth,contact.depth);
    return depth;
    }
  return 0;
  }

void DynamicWorld::damageMovableVob(BodyData& obj,float impulse,const Tempest::Vec3& point,FractureHit hit,
                                  std::string_view impactCulprit,const Tempest::Vec3& direction) {
  const bool arrow=hit==FractureHit::Arrow;
  if(!movableVobs->destructionEnabled || obj.fractureRule==nullptr || obj.movableShape->fracture==nullptr ||
     obj.body==nullptr || obj.pose->broken || obj.pendingBreak || !std::isfinite(impulse) || impulse<=0.f)
    return;
  const auto& rule=*obj.fractureRule;
  const float damage=arrow ? (impulse>=.5f ? impulse*rule.arrowDamage : 0.f) :
                            std::max(impulse/obj.movableMass-rule.threshold,0.f)*rule.impactDamage;
  if(damage<=0.f) return;
  obj.health=std::max(0.f,obj.health-damage);
  if(obj.health>.001f) return;
  obj.pendingBreak=true;
  obj.traceTime=traceTime;
  obj.traceCulprit=impactCulprit.empty() ? obj.body->traceCulprit() : impactCulprit;
  obj.breakImpulse=impulse;
  obj.breakPoint=point;
  obj.breakDirection=direction;
  obj.breakHit=hit;
  }

size_t DynamicWorld::applyMovableVobImpulse(const Tempest::Vec3& center, float radius,
                                           const std::function<Tempest::Vec3(const Tempest::Vec3&,float)>& impulse,
                                           Npc* culprit, const Tempest::Vec3* pointCm) {
  if(!std::isfinite(radius) || radius<=0.f || !impulse)
    return 0;
  size_t count = 0;
  for(auto* obj:movableVobs->bodies) {
    if(obj->body==nullptr) {
      // A broken prop: its remnants take the force and break.
      count+=spellRemnants(*obj,center,radius,impulse,culprit,pointCm);
      continue;
      }
    if(!movableVobs->enabled && obj->className!="fence")
      continue;
    const auto target = obj->body->centerOfMass();
    if((target-center).quadLength()>radius*radius)
      continue;
    const auto force = impulse(target,obj->movableMass);
    if(force.quadLength()==0.f)
      continue;
    std::vector<const CollisionWorld::CollisionBody*> excluded;
    excluded.reserve(obj->query.size());
    for(const auto& part:obj->query)
      excluded.push_back(part.get());
    if(world->rayCast(center,target,visualMask,false,nullptr,&excluded).hasHit)
      continue;
    if(obj->className=="fence")
      obj->body->setTraceCulprit(culprit);
    if(obj->className=="fence" || obj->body->addImpulse(force,culprit,pointCm)) {
      ++count;
      damageMovableVob(*obj,force.length(),pointCm!=nullptr ? *pointCm : target,FractureHit::Spell,{},force);
      }
    }
  return count;
  }

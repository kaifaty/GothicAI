#include "loadprobe.h"
#include "item.h"
#include "graphics/inventoryrenderer.h"
#include "graphics/mesh/protomesh.h"

#include <Tempest/Log>
#include <algorithm>
#include <cmath>

#include "game/serialize.h"
#include "settings.h"
#include "game/gamescript.h"
#include "world/objects/npc.h"
#include "world/world.h"
#include "utils/fileext.h"
#include "utils/dbgpainter.h"

using namespace Tempest;

namespace {

asset::MaterialGroup physicsMaterial(int32_t material) {
  // Script ItemMaterial values do not match the world mesh MaterialGroup enum.
  switch(material) {
    case MAT_WOOD:    return asset::MaterialGroup::WOOD;
    case MAT_STONE:   return asset::MaterialGroup::STONE;
    case MAT_METAL:   return asset::MaterialGroup::METAL;
    case MAT_LEATHER: return asset::MaterialGroup::WOOD;
    case MAT_CLAY:    return asset::MaterialGroup::EARTH;
    case MAT_GLAS:    return asset::MaterialGroup::STONE;
    default:         return asset::MaterialGroup::UNDEFINED;
    }
  }

}

Item::Item(World &owner, size_t itemInstance, Type type, const Tempest::Matrix4x4& initial)
  :Vob(owner) {
  LoadProbe::Startup::Scope startupProbe(LoadProbe::Startup::Item);
  LoadProbe::Startup::Scope::bytes(LoadProbe::Startup::Item,sizeof(Item));
  assert(itemInstance!=size_t(-1));
  hitem = owner.script().createItem(*this,itemInstance,itemInstanceSymbol);
  setCount(1);
  // Mass and native inertia are fixed when physics is created. The default
  // Vob transform is zero, so establish the final pose and scale first.
  setObjMatrix(initial);

  if(type!=T_Inventory) {
    const auto* residency=world.chunkResidency();
    Resources::TextureLoadScope textureLoadScope(residency &&
        residency->textureDistance(ChunkId::fromPosition(pos.x,pos.z))>2);
    if(!FileExt::hasExt(hitem->visual,"ZEN")) {
      visual.setWorldItem(*hitem,world,transform());
      proto=visual.protoMesh();
      }
    if(type==T_WorldDyn)
      setPhysicsEnable(proto);
    }
  }

Item::Item(World &owner, Serialize &fin, Type type)
  :Vob(owner) {
  hitem = owner.script().loadItem(fin,*this,itemInstanceSymbol);
  Tempest::Matrix4x4 mat;
  fin.read(amount);
  fin.read(pos,equipped,itSlot);
  fin.read(mat);
  setLocalTransform(mat);

  if(type!=T_Inventory) {
    const auto* residency=world.chunkResidency();
    Resources::TextureLoadScope textureLoadScope(residency &&
        residency->textureDistance(ChunkId::fromPosition(pos.x,pos.z))>2);
    if(!FileExt::hasExt(hitem->visual,"ZEN")) {
      visual.setWorldItem(*hitem,world,transform());
      proto=visual.protoMesh();
      }
    if(type==T_WorldDyn)
      setPhysicsEnable(proto);
    }

  visual.setObjMatrix(mat);
  physic.setObjMatrix(mat);
  }

Item::Item(Item &&it)
  : Vob(it.world), hitem(std::move(it.hitem)),itemInstanceSymbol(it.itemInstanceSymbol),
    pos(it.pos),amount(it.amount),equipped(it.equipped),itSlot(it.itSlot),
    placedZenId(it.placedZenId),placedAllowed(it.placedAllowed),proto(it.proto),visual(std::move(it.visual)) {
  world.script().attachItem(hitem,*this);
  setLocalTransform(it.localTransform());
  physic = std::move(it.physic);
  if(!physic.isEmpty())
    physic.setItem(this);
  }

Item::~Item() {
  }

void Item::drawVobBox(DbgPainter& p) const {
  p.setPen(Tempest::Color(1,0,0));
  if(proto!=nullptr) {
    p.drawObb(transform(), proto->bbox());

    auto v = midPosition();
    p.setPen(Tempest::Color(0,1,0));
    p.drawPoint(v);
    }
  }

void Item::drawVobRay(DbgPainter& p, const Npc& npc) const {
  if(auto bbox = this->bBox()) {
    // npc eyesight height
    auto  cen    = npc.mapHeadBone();
    auto  at     = this->midPosition();
    auto  tMax   = (at - cen).length();
    auto  dir    = (at - cen)/tMax;
    float tHit   = DynamicWorld::rayBox(cen, dir, tMax, transform(), bbox[0], bbox[1]);

    bool accessable = true;
    const auto r = world.physic()->ray(cen, cen+dir*tHit);
    if(r.hasCol)
      accessable = false;
    p.setPen(accessable ? Tempest::Color(0,1,0) : Tempest::Color(1,0,0));
    p.drawLine(cen, cen+dir*tHit);

    p.setPen(Tempest::Color(1,1,0));
    p.drawLine(cen+dir*tHit, at);
    }
  }

void Item::save(Serialize &fout) const {
  fout.write(uint32_t(clsId()),*hitem);
  fout.write(amount);
  fout.write(pos,equipped,itSlot);
  fout.write(localTransform());
  }

void Item::savePhysics(Serialize& fout) const {
  fout.write(uint32_t(clsId()),hitem->visual,localTransform(),physic.velocity(),
             physic.angularVelocity(),physic.isSleeping());
  }

void Item::restorePhysics(const Tempest::Matrix4x4& matrix, const Tempest::Vec3& velocity,
                          const Tempest::Vec3& angular, bool sleeping) {
  setPhysicsEnable(world);
  // Native restore preserves dry sleepers and wakes a saved float when
  // buoyancy is disabled, after body creation and transform callbacks.
  physic.restorePhysics(matrix,sleeping,velocity,angular);
  }

void Item::clearView() {
  visual = ObjVisual();
  }

bool Item::isTorchBurn() const {
  return false;
  }

void Item::setPosition(float x, float y, float z) {
  pos={x,y,z};
  updateMatrix();
  }

void Item::setDirection(float, float, float) {
  }

void Item::setObjMatrix(const Tempest::Matrix4x4 &m) {
  pos.x = m.at(3,0);
  pos.y = m.at(3,1);
  pos.z = m.at(3,2);
  setLocalTransform(m);
  visual.setObjMatrix(m);
  visual.setTextureOwner(pos,true);
  }

void Item::setPhysicsMatrix(const Tempest::Matrix4x4& m) {
  pos.x = m.at(3,0);
  pos.y = m.at(3,1);
  pos.z = m.at(3,2);
  // This pose already came from Jolt. Echoing it through moveEvent converts
  // its rotation/COM back to native coordinates and can reset the sleep timer
  // on rounding differences, keeping the whole floating contact island awake.
  setGlobalTransform(m);
  visual.setObjMatrix(m);
  visual.setTextureOwner(pos,true);
  }

bool Item::isMission() const {
  return (uint32_t(hitem->flags)&ITM_MISSION);
  }

void Item::setAsEquipped(bool e) {
  if(e)
    ++equipped; else
    --equipped;
  if(equipped>amount) {
    Log::e("[",displayName(),"] inconsistent inventory state");
    }
  if(equipped==0)
    itSlot=NSLOT;
  }

const ProtoMesh* Item::physicsProto() {
  if(proto==nullptr && !FileExt::hasExt(hitem->visual,"ZEN"))
    proto=Resources::loadMesh(hitem->visual);
  return proto;
  }

void Item::setPhysicsEnable(World& world) {
  setPhysicsEnable(physicsProto());
  world.invalidateVobIndex();
  }

void Item::setPhysicsDisable() {
  physic = DynamicWorld::Item();
  world.invalidateVobIndex();
  }

bool Item::wakePhysics() {
  if(Settings::physicsFlags().placedItems && isPlacedItem() && !placedAllowed)
    return false;
  const bool missing = !isDynamic();
  if(missing) {
    setPhysicsEnable(world);
    if(!isDynamic())
      return false;
    physic.restorePhysics(transform(),true);
    }
  physic.retainForItemWake();
  return missing;
  }

bool Item::activatePhysics() {
  if(Settings::physicsFlags().placedItems && isPlacedItem() && !placedAllowed)
    return false;
  wakePhysics();
  if(!isDynamic())
    return false;
  physic.restorePhysics(transform(),false);
  return true;
  }

void Item::setPhysicsEnable(const ProtoMesh* mesh) {
  if(Settings::physicsFlags().placedItems && isPlacedItem() && !placedAllowed)
    return;
  if(mesh==nullptr)
    return;
  auto& p = *world.physic();
  Bounds b;
  b.assign(mesh->bboxMesh);
  physic = p.addDynamicObj(transform(),b,physicsMaterial(hitem->material),
                           mesh,hitem->visual,Settings::physicsFlags().itemHulls);
  physic.setItem(this);
  }

bool Item::addSpellImpulse(const Vec3& impulseNs, Npc* culprit) {
  if(!std::isfinite(impulseNs.x) || !std::isfinite(impulseNs.y) || !std::isfinite(impulseNs.z) ||
     impulseNs.quadLength()<=0.f)
    return false;
  if(!isDynamic())
    setPhysicsEnable(world);
  const float mass = physic.mass();
  const float momentum = impulseNs.length();
  if(mass<=0.f || momentum<=0.f)
    return false;
  // Very light props receive the same force, with a bounded launch velocity.
  const auto impulse = impulseNs*std::min(1.f,12.f*mass/momentum);
  return physic.addImpulse(impulse,culprit);
  }

bool Item::isDynamic() const {
  return !physic.isEmpty();
  }

std::string_view Item::displayName() const {
  return hitem->name;
  }

std::string_view Item::description() const {
  return hitem->description;
  }

Vec3 Item::displayPosition() const {
  if(auto b = bBox())
    return pos + Vec3(0, b[1].y, 0);
  return pos;
  }

Tempest::Vec3 Item::position() const {
  return pos;
  }

const Vec3* Item::bBox() const {
  if(proto==nullptr)
    return nullptr;
  return proto->bbox();
  }

Vec3 Item::midPosition() const {
  if(proto!=nullptr) {
    auto bbox = proto->bbox();
    auto v    = (bbox[0] + bbox[1])*0.5;
    transform().project(v); // doesn't use to work for Karibik mod
    return v;
    }
  return pos;
  }

bool Item::isGold() const {
  return itemInstanceSymbol==world.script().goldId()->index();
  }

ItmFlags Item::mainFlag() const {
  return ItmFlags(hitem->main_flag);
  }

int32_t Item::itemFlag() const {
  return static_cast<int>(hitem->flags);
  }

bool Item::isMulti() const {
  return uint32_t(hitem->flags)&ITM_MULTI;
  }

bool Item::isSpellShoot() const {
  if(!isSpellOrRune())
    return false;
  auto& spl = world.script().spellDesc(spellId());
  return spl.target_collect_algo!=TargetCollect::TARGET_COLLECT_NONE &&
         spl.target_collect_algo!=TargetCollect::TARGET_COLLECT_CASTER &&
         spl.target_collect_algo!=TargetCollect::TARGET_COLLECT_FOCUS &&
         spl.target_collect_algo!=TargetCollect::TARGET_COLLECT_ALL_FALLBACK_NONE;
  }

bool Item::isSpellOrRune() const {
  return (uint32_t(mainFlag()) & ITM_CAT_RUNE);
  }

bool Item::isSpell() const {
  if(isSpellOrRune())
    return isMulti();
  return false;
  }

bool Item::isRune() const {
  return isSpellOrRune() && !isSpell();
  }

bool Item::is2H() const {
  auto flg = uint32_t(itemFlag());
  return flg & (ITM_2HD_SWD | ITM_2HD_AXE);
  }

bool Item::isCrossbow() const {
  auto flg = uint32_t(itemFlag());
  return flg & ITM_CROSSBOW;
  }

bool Item::isRing() const {
  auto flg = uint32_t(itemFlag());
  return flg & ITM_RING;
  }

bool Item::isArmor() const {
  auto flg = ItmFlags(mainFlag());
  return flg & ITM_CAT_ARMOR;
  }

int32_t Item::spellId() const {
  return hitem->spell;
  }

int32_t Item::swordLength() const {
  return hitem->range;
  }

void Item::setCount(size_t cnt) {
  amount = uint32_t(cnt);
  }

size_t Item::count() const {
  return amount;
  }

std::string_view Item::uiText(size_t id) const {
  return hitem->text[id];
  }

int32_t Item::uiValue(size_t id) const {
  return hitem->count[id];
  }

int32_t Item::cost() const {
  return hitem->value;
  }

int32_t Item::sellCost() const {
  return int32_t(std::ceil(world.script().tradeValueMultiplier()*float(cost())));
  }

bool Item::checkCond(const Npc &other) const {
  int32_t a=0,v=0;
  return checkCondUse(other,a,v) && checkCondRune(other,a,v);
  }

bool Item::checkCondUse(const Npc &other, int32_t &a, int32_t &nv) const {
  for(size_t i=0;i<ItemRecord::condition_count;++i){
    auto atr = Attribute(hitem->cond_atr[i]);
    if(other.attribute(atr)<hitem->cond_value[i] && hitem->cond_value[i]!=0) {
      a  = atr;
      nv = hitem->cond_value[i];
      return false;
      }
    }
  return true;
  }

bool Item::checkCondRune(const Npc &other, int32_t &cPl, int32_t &cIt) const {
  cIt = hitem->mag_circle;
  cPl = other.mageCycle();
  return (cPl>=cIt);
  }

size_t Item::clsId() const {
  return itemInstanceSymbol;
  }

void Item::updateMatrix() {
  Tempest::Matrix4x4 mat;
  mat.identity();
  mat.translate(pos.x,pos.y,pos.z);
  setLocalTransform(mat);
  }

void Item::moveEvent() {
  visual.setObjMatrix(transform());
  visual.setTextureOwner(pos,true);
  physic.setObjMatrix(transform());
  if(!isDynamic())
    world.invalidateVobIndex();
  }

void Item::drawInventory(InventoryRenderer& renderer, int x, int y, int w, int h) const {
  auto& itData = handle();
  if(auto mesh = Resources::loadMesh(itData.visual)) {
    auto     bbox = mesh->bbox();
    float    sz   = (bbox[1]-bbox[0]).length();
    auto     mv   = (bbox[1]+bbox[0])*0.5f;
    ItmFlags flg  = ItmFlags(mainFlag());

    mv = Vec3(mv.x,mv.y,mv.z);

    sz = 2.f/sz;
    if(sz>0.1f)
      sz=0.1f;

    Tempest::Matrix4x4 mat;
    mat.identity();

    float rotx = float(itData.inv_rot_x);
    float roty = float(itData.inv_rot_y);
    float rotz = float(itData.inv_rot_z);

    if(flg&(ITM_CAT_NF | ITM_CAT_FF | ITM_CAT_MUN)) {
      static const float invX = -45;
      static const float invY = 0;
      static const float invZ = 90;
      mat.rotateOX(invX+rotx);
      mat.rotateOZ(invZ+rotz);
      mat.rotateOY(invY+roty);
      }
    else if(flg&ITM_CAT_ARMOR) {
      static const float invX = 0;
      static const float invY = -90;
      static const float invZ = 180;
      mat.rotateOX(invX+rotx);
      mat.rotateOZ(invZ+rotz);
      mat.rotateOY(invY+roty);
      }
    else if(flg&ITM_CAT_RUNE) {
      static const float invX = 90;
      static const float invY = 0;
      static const float invZ = 90;
      mat.rotateOX(invX+rotx);
      mat.rotateOZ(invZ+rotz);
      mat.rotateOY(invY+roty);
      }
    else if((flg&ITM_CAT_MAGIC) || (ItmFlags(itData.flags)&ITM_RING)) {
      static const float invX = 200;
      static const float invY = 0;
      static const float invZ = 90;
      mat.rotateOX(invX+rotx);
      mat.rotateOZ(invZ+rotz);
      mat.rotateOY(invY+roty);
      }
    else if(flg&ITM_CAT_POTION) {
      static const float invX = 180;
      static const float invY = 0;
      static const float invZ = 0;
      mat.rotateOX(invX+rotx);
      mat.rotateOZ(invZ+rotz);
      mat.rotateOY(invY+roty);
      }
    else if(flg&ITM_CAT_FOOD) {
      static const float invX = 180;
      static const float invY = 0;
      static const float invZ = 45;
      mat.rotateOX(invX+rotx);
      mat.rotateOZ(invZ+rotz);
      mat.rotateOY(invY+roty);
      }
    else if(flg&ITM_CAT_DOCS) {
      static const float invX = 180;
      static const float invY = 90;
      static const float invZ = -90;
      mat.rotateOX(invX);
      mat.rotateOZ(invZ);
      mat.rotateOY(invY);
      }
    else if(flg&ITM_CAT_NONE) {
      static float invXa = 135;
      static float invYa = 90;
      static float invZa = 45;
      //invXa++;
      mat.rotateOX(invXa-rotx);
      mat.rotateOZ(invZa-rotz);
      mat.rotateOY(invYa-roty);
      } else {
      static const float invX = 180;
      static const float invY = -90;
      static const float invZ = -90;
      mat.rotateOX(invX+rotx);
      mat.rotateOZ(invZ+rotz);
      mat.rotateOY(invY+roty);
      }

    for(int i=0; i<3; ++i){
      auto trX = mat.at(i,0);
      auto trY = mat.at(i,2);
      mat.set(i,0,trY);
      mat.set(i,2,trX);
      }
    mat.scale(sz);
    mat.translate(-mv);

    renderer.drawItem(x,y,w,h,*mesh,itData.material,mat);
    }
  }

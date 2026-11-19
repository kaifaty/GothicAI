#include "itemview.h"

#include <Tempest/Log>

#include <cstdlib>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>

#include "gameservices.h"
#include "resources.h"
#include "settings.h"
#include "graphics/worldview.h"
#include "world/records/itemrecord.h"
#include "world/world.h"

namespace {

// the views with a live effect, for ItemView::census (the bench's itemfx)
struct Live {
  std::mutex                         sync;
  std::unordered_set<const ItemView*> views;
  // effect names Gothic does not know: looked up (and logged by it) once each
  std::unordered_set<std::string>    unknown;
  };

Live& live() {
  static Live l;
  return l;
  }

const VisualFx* effectOf(std::string_view name) {
  if(name.empty() || !ItemView::enabled())
    return nullptr;
  auto& l = live();
  {
    std::lock_guard<std::mutex> guard(l.sync);
    if(l.unknown.find(std::string(name))!=l.unknown.end())
      return nullptr;
  }
  // an unknown name is one line in the log: VisualFxDefinitions writes it, the second lookup does not happen
  auto* fx = GameServices::inst().loadVisualFx(name);
  if(fx==nullptr) {
    std::lock_guard<std::mutex> guard(l.sync);
    l.unknown.emplace(name);
    }
  return fx;
  }

}

ItemView::ItemView(World& owner, const ItemRecord& item, bool shown)
  :world(&owner), proto(Resources::loadMesh(item.visual)), material(item.material), fx(effectOf(item.effect)) {
  if(!shown)
    return;
  mesh = owner.addView(item);
  startEffect();
  }

ItemView::ItemView(const ProtoMesh* p, int32_t mat, MeshObjects::Mesh&& m)
  :proto(p), material(mat), mesh(std::move(m)) {
  }

ItemView::ItemView(ItemView&& other) noexcept {
  moveFrom(other);
  }

ItemView& ItemView::operator=(ItemView&& other) noexcept {
  if(this!=&other) {
    stopEffect();
    moveFrom(other);
    }
  return *this;
  }

ItemView::~ItemView() {
  stopEffect();
  }

void ItemView::moveFrom(ItemView& other) {
  world    = other.world;
  proto    = other.proto;
  material = other.material;
  fx       = other.fx;
  pos      = other.pos;
  mesh     = std::move(other.mesh);
  if(!other.hasEffect()) {
    effect = Effect();
    return;
    }
  {
    auto& l = live();
    std::lock_guard<std::mutex> guard(l.sync);
    l.views.erase(&other);
    l.views.insert(this);
  }
  effect = std::move(other.effect);
  other.effect = Effect();
  // the particles are born on this mesh now, not on the moved-from one
  effect.setMesh(&mesh);
  }

bool ItemView::enabled() {
  static const bool envOff = [] {
    const char* env = std::getenv("OPENGOTHIC_ITEM_EFFECTS");
    return env!=nullptr && std::string_view(env)=="0";
    }();
  if(envOff)
    return false;
  return !Settings::exists() || Settings::getI("GAME","itemEffects")!=0;
  }

ItemView::Census ItemView::census(const World& owner) {
  Census ret;
  auto& l = live();
  std::lock_guard<std::mutex> guard(l.sync);
  for(auto* v:l.views) {
    if(v->world!=&owner)
      continue;
    ++ret.alive;
    if(owner.isInPfxRange(v->effect.position()))
      ++ret.near;
    }
  return ret;
  }

bool ItemView::effectPosition(Tempest::Vec3& out) const {
  if(!hasEffect())
    return false;
  out = effect.position();
  return true;
  }

void ItemView::setObjMatrix(const Tempest::Matrix4x4& m) {
  pos = m;
  mesh.setObjMatrix(m);
  if(hasEffect())
    effect.setObjMatrix(m);
  }

float ItemView::setTextureOwner(const Tempest::Vec3& position, bool spatial) {
  return mesh.setTextureOwner(position,spatial);
  }

void ItemView::tick(uint64_t dt) {
  if(hasEffect())
    effect.tick(dt);
  }

void ItemView::release() {
  stopEffect();
  mesh = MeshObjects::Mesh();
  }

void ItemView::restore(WorldView& view) {
  if(!mesh.isEmpty() || proto==nullptr)
    return;
  mesh = view.addView(proto,material);
  mesh.setObjMatrix(pos);
  startEffect();
  }

void ItemView::startEffect() {
  if(fx==nullptr || world==nullptr || mesh.isEmpty())
    return;
  // as MdlVisual::setNpcEffect: active, looped, the particles born on the item's own mesh
  effect = Effect(*fx,*world,Tempest::Vec3(pos.at(3,0),pos.at(3,1),pos.at(3,2)),SpellFxKey::Count);
  effect.setPhysicsDisable();
  effect.setActive(true);
  effect.setLooped(true);
  effect.setMesh(&mesh);
  effect.setObjMatrix(pos);
  auto& l = live();
  std::lock_guard<std::mutex> guard(l.sync);
  l.views.insert(this);
  }

void ItemView::stopEffect() {
  if(!hasEffect())
    return;
  {
    auto& l = live();
    std::lock_guard<std::mutex> guard(l.sync);
    l.views.erase(this);
  }
  effect.setActive(false);
  effect = Effect();
  }

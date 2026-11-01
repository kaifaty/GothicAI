#include "itemtorchburning.h"

#include "world/world.h"

ItemTorchBurning::ItemTorchBurning(World& owner, size_t inst, Item::Type type, const Tempest::Matrix4x4& initial)
  :Item(owner,inst,type,initial) {
  const auto* residency=owner.chunkResidency();
  const auto at=position();
  Resources::TextureLoadScope textureLoadScope(residency &&
      residency->textureDistance(ChunkId::fromPosition(at.x,at.z))>2);
  view.setVisual(handle(),owner,false);
  view.setTextureOwner(at,true);
  view.setObjMatrix(transform());
  setPhysicsEnable(owner);
  }

ItemTorchBurning::ItemTorchBurning(Item&& loaded)
  :Item(std::move(loaded)) {
  // The legacy Item stream has already restored the script fields and pose.
  // Its optional physics entry will create the native body afterwards.
  const auto* residency=world.chunkResidency();
  const auto at=position();
  Resources::TextureLoadScope textureLoadScope(residency &&
      residency->textureDistance(ChunkId::fromPosition(at.x,at.z))>2);
  view.setVisual(handle(),world,false);
  view.setTextureOwner(at,true);
  view.setObjMatrix(transform());
  }

void ItemTorchBurning::setPhysicsEnable(World& owner) {
  auto& sc = owner.script();
  size_t torchId = sc.findSymbolIndex("ItLsTorchburned");
  if(torchId!=size_t(-1)) {
    auto hitem = std::make_shared<zenkit::IItem>();
    sc.initializeInstanceItem(hitem, torchId);
    auto m = Resources::loadMesh(hitem->visual);
    Item::setPhysicsEnable(m);
    }
  owner.invalidateVobIndex();
  }

void ItemTorchBurning::clearView() {
  Item::clearView();
  view = ObjVisual();
  }

bool ItemTorchBurning::isTorchBurn() const {
  return true;
  }

void ItemTorchBurning::setPhysicsMatrix(const Tempest::Matrix4x4& matrix) {
  Item::setPhysicsMatrix(matrix);
  view.setObjMatrix(transform());
  view.setTextureOwner(position(),true);
  }

void ItemTorchBurning::moveEvent() {
  Item::moveEvent();
  view.setObjMatrix(transform());
  view.setTextureOwner(position(),true);
  }

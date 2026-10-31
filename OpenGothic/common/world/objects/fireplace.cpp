#include "fireplace.h"

#include "asset/worldtypes.h"

FirePlace::FirePlace(Vob* parent, World& world, const asset::VFire& vob, Flags flags)
  : Interactive(parent,world,vob,flags){
  fireVobtreeName = vob.vob_tree;
  fireSlot        = vob.slot;
  }

void FirePlace::load(Serialize& fin) {
  Interactive::load(fin);
  onStateChanged();
  }

void FirePlace::moveEvent() {
  Interactive::moveEvent();

  auto at = this->mapBone(fireSlot);
  fireVobtree.setObjMatrix(at);
  }

void FirePlace::onStateChanged() {
  if(stateId()>0) {
    auto at = this->mapBone(fireSlot);
    fireVobtree = VobBundle(world,fireVobtreeName,Vob::Startup);
    fireVobtree.setObjMatrix(at);
    } else {
    fireVobtree = VobBundle();
    }
  }

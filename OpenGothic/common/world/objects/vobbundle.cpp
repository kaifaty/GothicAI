#include "vobbundle.h"

#include "world/world.h"
#include "world/voblayer.h"
#include "resources.h"
#include "worldbase.h"

VobBundle::VobBundle(World& owner, std::string_view filename, Vob::Flags flags) {
  auto* bundle = loadVobBundle(filename);
  for(auto& vob:*bundle) {
    rootVobs.emplace_back(Vob::load(nullptr,owner,*vob,(flags | Vob::Startup)));
    if(VobLayer::isAdded(*vob)) {
      slotLocalTransforms.push_back(rootVobs.back()->transform());
      slotUp.push_back(VobLayer::upCm(*vob));
      }
    }
  }

void VobBundle::setObjMatrix(const Tempest::Matrix4x4& obj) {
  for(size_t i=0;i<rootVobs.size();++i) {
    auto transform=obj;
    if(!slotLocalTransforms.empty()) {
      transform.mul(slotLocalTransforms[i]);
      transform.set(3,1,transform.at(3,1)+slotUp[i]);
      }
    rootVobs[i]->setLocalTransform(transform);
    }
  }

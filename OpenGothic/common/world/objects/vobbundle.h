#pragma once

#include <memory>

#include "vob.h"

class VobBundle {
  public:
    VobBundle() = default;
    VobBundle(World& owner, std::string_view filename, Vob::Flags flags);

    void setObjMatrix(const Tempest::Matrix4x4& obj);

  private:
    friend struct WorldDataContract;
    std::vector<std::unique_ptr<Vob>> rootVobs;
    std::vector<Tempest::Matrix4x4> slotLocalTransforms;
  };


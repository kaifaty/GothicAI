#pragma once

#include "physics/physics.h"
#include "physicvbo.h"

class PhysicMeshShape final {
  public:
    PhysicMeshShape(const PhysicMeshShape&)=delete;

    float friction() const;

    static PhysicMeshShape* load(PackedMesh&& packed);

  private:
    PhysicMeshShape(PackedMesh&& packed);

    PhysicVbo                      mesh;
    float                          frict = 0;
    asset::MaterialGroup           material = asset::MaterialGroup::UNDEFINED;

  friend class DynamicWorld;
  };

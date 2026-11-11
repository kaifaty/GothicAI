#pragma once

#include <cstdint>
#include "graphics/meshobjects.h"

class DynamicWorld;

class ClothWorld {
  public:
    virtual ~ClothWorld() = default;
    virtual uint64_t tickCount() const = 0;
    virtual DynamicWorld* physic() const = 0;
    virtual MeshObjects::Mesh addStaticView(const ProtoMesh* visual, bool staticDraw) = 0;
  };

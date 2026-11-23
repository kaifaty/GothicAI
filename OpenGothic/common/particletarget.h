#pragma once

#include <Tempest/Matrix4x4>

class ParticleTarget {
  public:
    virtual ~ParticleTarget() = default;
    virtual Tempest::Matrix4x4 transform() const = 0;
  };

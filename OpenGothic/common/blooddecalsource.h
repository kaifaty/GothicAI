#pragma once

#include <vector>
#include "bloodstain.h"

class BloodDecalSource {
  public:
    virtual ~BloodDecalSource() = default;
    virtual const std::vector<BloodStains::Stain>& bloodStains() const = 0;
    virtual int64_t bloodTime() const = 0;
  };

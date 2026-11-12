#pragma once

#include "records/definitions.h"
#include <string_view>

class CameraPresets {
  public:
    virtual ~CameraPresets() = default;

    virtual const Def::Camera& dialogCam() const = 0;
    virtual const Def::Camera& inventoryCam() const = 0;
    virtual const Def::Camera& stdCam() const = 0;
    virtual const Def::Camera& fpCam() const = 0;
    virtual const Def::Camera& backCam() const = 0;
    virtual const Def::Camera& meleeCam() const = 0;
    virtual const Def::Camera& rangeCam() const = 0;
    virtual const Def::Camera& mageCam() const = 0;
    virtual const Def::Camera& deathCam() const = 0;
    virtual const Def::Camera& swimCam() const = 0;
    virtual const Def::Camera& diveCam() const = 0;
    virtual const Def::Camera& fallCam() const = 0;
    virtual const Def::Camera& mobsiCam(std::string_view tag, std::string_view pos) const = 0;
  };

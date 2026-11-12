#pragma once

#include <Tempest/Matrix4x4>
#include <string_view>

class CameraTarget {
  public:
    virtual ~CameraTarget() = default;

    virtual Tempest::Vec3 cameraTargetBone() const = 0;
    virtual float cameraTargetRotation() const = 0;
    virtual float cameraTargetRotationRad() const = 0;
    virtual Tempest::Matrix4x4 cameraTargetMatrix(bool firstPerson) const = 0;
    virtual bool cameraTargetSwim() const = 0;
    virtual bool cameraTargetInAir() const = 0;
    virtual bool cameraTargetJump() const = 0;
    virtual bool cameraTargetDead() const = 0;
    virtual std::string_view cameraTargetScheme() const = 0;
    virtual std::string_view cameraTargetPosScheme() const = 0;
  };

#pragma once

#include <Tempest/Matrix4x4>
#include <Tempest/Point>

#include "asset/modeltypes.h"

asset::AnimationSample mix(const asset::AnimationSample& x, const asset::AnimationSample& y, float a);
zenkit::Quat            slerp(const zenkit::Quat& x, const zenkit::Quat& y, float a);
Tempest::Matrix4x4      mkMatrix(const asset::AnimationSample& s);

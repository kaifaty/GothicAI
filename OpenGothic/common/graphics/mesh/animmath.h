#pragma once

#include <Tempest/Matrix4x4>
#include <Tempest/Point>

#include "asset/modeltypes.h"

asset::AnimationSample mix(const asset::AnimationSample& x, const asset::AnimationSample& y, float a);
asset::Quat            slerp(const asset::Quat& x, const asset::Quat& y, float a);
Tempest::Matrix4x4      mkMatrix(const asset::AnimationSample& s);

#pragma once

#include <Tempest/Matrix4x4>

namespace MdlVisualInternal {

bool inverseBasis(const Tempest::Matrix4x4& m, double inverse[3][3]);
bool validRenderFrame(const Tempest::Matrix4x4& m);

}

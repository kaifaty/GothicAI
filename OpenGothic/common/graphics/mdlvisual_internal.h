#pragma once

#include <Tempest/Matrix4x4>
#include <Tempest/Vec>

namespace MdlVisualInternal {

bool inverseBasis(const Tempest::Matrix4x4& m, double inverse[3][3]);
bool validRenderFrame(const Tempest::Matrix4x4& m);
// thinking of an answer: the point aside and up of the one it talks to (TalkGestures::eyesAside), turned around
// `head` by yaw (+ to the left as Gaze turns) and pitch, radians
Tempest::Vec3 asidePoint(const Tempest::Vec3& head, const Tempest::Vec3& target, float yaw, float pitch);

constexpr float EyeAsideHead = 0.3f;  // a head with eyes of its own: of the look aside the head takes this part
constexpr float EyesUp       = 11.f;  // cm: the eyes of a head of the original above its bone, along its up (x)

}

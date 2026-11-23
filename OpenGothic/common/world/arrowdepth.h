#pragma once

#include <Tempest/Matrix4x4>
#include <Tempest/Vec>

#include "asset/material.h"

class ProtoMesh;

// How deep a stuck arrow's tip goes past the point it entered
// (reports/physics/2026-11-19-arrow-pin-depth): the arrow's mesh lies along X, its tip on −X, and −X of a
// projectile's matrix is its flight. Only the visible arrow moves; the hit, its point and the save do not.
namespace ArrowDepth {
  // into a body, living or lying, past the entry into the hit zone
  constexpr float Body = 12.f;

  // into the world by the surface's material group: wood 8, stone and metal 2, earth, snow and the rest 15
  float world(asset::MaterialGroup group);

  // from the mesh's origin to its tip along the flight, by its box: 0 when the mesh or its box is unknown
  float tipLength(const ProtoMesh* mesh);

  // A world stop: an arrow that glanced off stone and stopped has the reflected flight in its matrix, the tip
  // away from the surface. Turned to the flight that came into it, `into`, when its tip does not point into the
  // surface (`inward`: the normal facing away from the shooter's side).
  void faceSurface(Tempest::Matrix4x4& m, const Tempest::Vec3& into, const Tempest::Vec3& inward);

  // The projectile's matrix moved back along its flight so the tip, `tipLength` from the mesh's origin, is
  // `depth` past `entry`. Unchanged when the tip's length is unknown (0): the arrow stays as before.
  void sink(Tempest::Matrix4x4& m, const Tempest::Vec3& entry, float depth, float tipLength);
  }

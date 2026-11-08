#pragma once

#include "physics/physics.h"
#include "asset/material.h"
#include <Tempest/Vec>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <Jolt/Physics/Collision/GroupFilter.h>

// Offline cache geometry in source-visual centimetres. Piece transforms use
// these coordinates directly, so texture coordinates remain unchanged.
struct FractureMesh {
  struct Vertex {
    Tempest::Vec3 position, normal;
    Tempest::Vec2 uv;
    };
  struct Triangle {
    std::string texture;
    Vertex vertices[3];
    };
  struct Piece {
    double volume = 0; // cubic centimetres
    Tempest::Vec3 center;
    asset::MaterialGroup group = asset::MaterialGroup::UNDEFINED;
    std::vector<Triangle> triangles;
    JPH::ShapeRefC hull;
    };
  std::vector<Piece> pieces;
  double volume = 0;
  size_t version = 0;

  // Originals use v1 from OPENGOTHIC_FRACTURE, otherwise ~/.local/share/gothic-ai/fracture.
  // Loaded GLB replacements use only their v2/v3 sidecar, matched to the loaded bytes.
  static std::shared_ptr<const FractureMesh> load(std::string_view visual);
  };

// Format 3 pieces of one break: METAL hoops and bands never touch WOOD staves
// and boards, so the hoops fall free as the wood spreads. Everything else collides.
bool fractureMaterialsCollide(asset::MaterialGroup a, asset::MaterialGroup b);
// Native filter of one format 3 break: the group ID is the break, the sub-group the
// piece index, and the table disables only the pairs fractureMaterialsCollide rejects.
JPH::Ref<JPH::GroupFilter> fractureGroupFilter(const FractureMesh& mesh);

struct FractureRule {
  enum class Melee { Fist, OneHand, TwoHand };
  std::string visualPattern, className;
  float health = 100;
  float threshold = 5; // newton seconds per kilogram (delta velocity, m/s)
  float impactDamage = 250; // health / (Ns/kg) above threshold
  float arrowDamage = 8.333334f; // health / Ns; 0.12 kg arrow at 30 m/s => 30 health
  float meleeWeapon = .5f, meleeStrength = .5f;
  float fist = .35f, oneHand = 1.f, twoHand = 1.3f;
  float meleeDamage(Melee kind, float weaponDamage, float strength) const;
  static const FractureRule* classify(std::string_view visual);
  };

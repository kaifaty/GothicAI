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
    // Format 3: the convex hulls of the piece (hull is one of them or their compound).
    std::vector<JPH::ShapeRefC> hulls;
    };
  // Format 3 bonds (reports/physics/2026-11-destruction-bonds): pieces a < b joined at
  // `point`, source-visual centimetres. A file without the block has none.
  enum class BondKind : uint8_t { Nail, Hoop, Groove, Dowel, Along, Across };
  static constexpr size_t BondKinds = 6;
  struct Bond {
    uint32_t a = 0, b = 0;
    BondKind kind = BondKind::Nail;
    Tempest::Vec3 point;
    };
  std::vector<Piece> pieces;
  std::vector<Bond> bonds;
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
// The same between the bond groups of one break (sub-group = group index): a group of
// METAL pieces only does not collide with the groups that hold WOOD.
JPH::Ref<JPH::GroupFilter> fractureGroupFilter(const FractureMesh& mesh, const std::vector<uint32_t>& pieceGroup,
                                               uint32_t groups);

// What broke the prop: the bond radius depends on it, a spell's force breaks every bond.
enum class FractureHit : uint8_t { Fist, OneHand, TwoHand, Arrow, Impact, Spell };

// Bond rule numbers, OPENGOTHIC_FRACTURE_BONDS="fist=20,one=35,two=55,arrow=12,impact=30,
// nail=1,hoop=1,groove=1.2,dowel=0.8,along=0.7,across=1.5,ring=0.5,remnant=0.5"; an invalid
// entry logs one error and keeps every default.
struct FractureBonds {
  float radius[5] = {20.f, 35.f, 55.f, 12.f, 30.f}; // centimetres by FractureHit, Fist..Impact
  float strength[FractureMesh::BondKinds] = {1.f, 1.f, 1.2f, .8f, .7f, 1.5f}; // by BondKind
  float ring = .5f; // a hoop that lost more than this share of its HOOP bonds lets go of all
  // A remnant (pieces still joined) holds class health x its volume share x remnant; 0 breaks on every blow.
  float remnant = .5f;
  static FractureBonds load();
  };

// The bond groups left after a blow of `hit` at `point` (source-visual centimetres): group
// index per piece, groups numbered by their smallest piece. Without bonds or for a spell,
// every piece is its own group (group == piece). `broken`, when given, gets each bond's fate.
std::vector<uint32_t> fractureGroups(const FractureMesh& mesh, const FractureBonds& rule, FractureHit hit,
                                     const Tempest::Vec3& point, std::vector<bool>* broken = nullptr);
// The same rule over one remnant: `members` are its pieces, `broken` every bond's fate so far,
// updated in place. Only bonds still intact inside the remnant can break; the struck piece is the
// nearest member not of METAL; a hoop's ring counts its HOOP bonds broken by earlier blows too.
void fractureCut(const FractureMesh& mesh, const FractureBonds& rule, FractureHit hit, const Tempest::Vec3& point,
                 const std::vector<uint32_t>& members, std::vector<bool>& broken);
// Bond groups joined by the intact bonds: group index per piece, numbered by their smallest piece.
std::vector<uint32_t> fractureComponents(const FractureMesh& mesh, const std::vector<bool>& broken);

struct FractureRule {
  enum class Melee { Fist, OneHand, TwoHand };
  static FractureHit hit(Melee kind);
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

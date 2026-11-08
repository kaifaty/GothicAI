#pragma once

#include "dynamicworld.h"
#include "collisionworld.h"
#include "convexparts.h"

#include <Tempest/Log>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <locale>
#include <sstream>
#include <unordered_map>

namespace DynamicWorldInternal {
constexpr uint32_t landscapeMask = (1u << DynamicWorld::C_Landscape) | (1u << DynamicWorld::C_Object) |
                                    (1u << DynamicWorld::C_MovableQuery);
constexpr uint32_t visualMask    = landscapeMask | (1u << DynamicWorld::C_SmallVobQuery);

struct FractureMotion {
  float hit = 150.f, falloff = 60.f, outward = 120.f, spin = 3.f, cap = 200.f;

  static FractureMotion load() {
    FractureMotion motion;
    const auto* env=std::getenv("OPENGOTHIC_FRACTURE_MOTION");
    if(env==nullptr || env[0]=='\0') return motion;
    const auto invalid=[](const std::string& entry, const char* reason) {
      Tempest::Log::e("Invalid OPENGOTHIC_FRACTURE_MOTION entry '",entry,"': ",reason,"; using defaults");
      return FractureMotion{};
      };
    std::istringstream entries(env);
    for(std::string entry;std::getline(entries,entry,',');) {
      std::istringstream field(entry);
      field.imbue(std::locale::classic());
      std::string key;
      float value=0;
      if(!std::getline(field,key,'=') || !(field>>value) || !(field>>std::ws).eof() ||
         !std::isfinite(value) || value<0.f)
        return invalid(entry,"expected key=value with a finite non-negative number");
      const auto first=key.find_first_not_of(" \t\r\n\f\v");
      if(first==std::string::npos) return invalid(entry,"missing key");
      key=key.substr(first,key.find_last_not_of(" \t\r\n\f\v")-first+1);
      if(key=="hit") motion.hit=value;
      else if(key=="falloff") {
        if(value==0.f) return invalid(entry,"falloff must be greater than zero");
        motion.falloff=value;
        }
      else if(key=="outward") motion.outward=value;
      else if(key=="spin") motion.spin=value;
      else if(key=="cap") motion.cap=value;
      else return invalid(entry,"unknown key");
      }
    return motion;
    }
  };

inline ItemMaterial impactMaterial(asset::MaterialGroup material) {
  switch(material) {
    case asset::MaterialGroup::METAL: return MAT_METAL;
    case asset::MaterialGroup::STONE: return MAT_STONE;
    case asset::MaterialGroup::WOOD:  return MAT_WOOD;
    default:                          return MAT_CLAY;
    }
  }

inline double volumeScale(const Tempest::Matrix4x4& matrix) {
  const auto* m = matrix.data();
  return std::abs(double(m[0])*(double(m[5])*m[10]-double(m[9])*m[6])-
                  double(m[4])*(double(m[1])*m[10]-double(m[9])*m[2])+
                  double(m[8])*(double(m[1])*m[6]-double(m[5])*m[2]));
  }

}

using namespace DynamicWorldInternal;

struct DynamicWorld::MovableVobs {
  struct Part {
    const PhysicMeshShape* source = nullptr;
    Tempest::Matrix4x4    local;
    };
  struct Shape {
    JPH::ShapeRefC              hull;
    std::vector<Part>           parts;
    asset::MaterialGroup      material = asset::MaterialGroup::UNDEFINED;
    float                      friction = 0.f;
    bool                       hullAttempted = false;
    bool                       convexPartsAttempted = false;
    std::shared_ptr<const ConvexParts> convexParts;
    std::shared_ptr<const FractureMesh> fracture;
    };
  std::unordered_map<std::string,std::shared_ptr<Shape>> shapes;
  std::vector<BodyData*> bodies;
  std::vector<MovableVobContacts> census;
  const FractureMotion fractureMotion = FractureMotion::load();
  const FractureBonds fractureBonds = FractureBonds::load();
  double validationMs = 0;
  bool finalized = false;
  bool enabled = true;
  bool furniturePartsEnabled = true;
  bool destructionEnabled = true;
  };

struct DynamicWorld::BodyData {
  ~BodyData() {
    if(movableOwner!=nullptr) {
      auto& bodies = movableOwner->bodies;
      bodies.erase(std::remove(bodies.begin(),bodies.end(),this),bodies.end());
      }
    }
  std::unique_ptr<CollisionWorld::CollisionBody> body;
  MovableVobs* movableOwner = nullptr;
  std::shared_ptr<MovableVobs::Shape> movableShape;
  std::vector<std::unique_ptr<CollisionWorld::CollisionBody>> query;
  Tempest::Matrix4x4 origin, current;
  std::shared_ptr<MovableVobPose> pose = std::make_shared<MovableVobPose>();
  std::string visual, className;
  std::string disposition;
  float movableMass = 0.f, massMultiplier = 1.f;
  bool blocksWalking = false, legacyCollision = true;
  bool wasSleeping = true;
  Tempest::Vec3 localBounds[2];
  bool trace = false, tilted = false;
  int64_t traceTime = -1;
  std::string traceCulprit;
  std::function<void(const Tempest::Matrix4x4&)> transformCallback;
  const FractureRule* fractureRule = nullptr;
  float health = 0, breakImpulse = 0;
  Tempest::Vec3 breakPoint, breakDirection;
  FractureHit breakHit = FractureHit::Impact;
  bool pendingBreak = false, fragmentsWereSleeping = true;
  // One native body per bond group (reports/physics/2026-11-destruction-bonds): fragments,
  // their sleep time and permanence are per group, fragmentGroup maps each piece to its
  // group. Without bonds every piece is its own group, numbered as the piece.
  std::vector<std::unique_ptr<CollisionWorld::DynamicBody>> fragments;
  std::vector<int64_t> fragmentSleepTime;
  std::vector<bool> fragmentPermanent;
  std::vector<uint32_t> fragmentGroup;
  std::function<void(bool,const std::vector<Tempest::Matrix4x4>&)> fractureCallback;
  };

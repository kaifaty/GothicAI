#pragma once

#include <Tempest/Vec>
#include "asset/material.h"
#include "game/constants.h"
#include <cstdint>
#include <string>

class Npc;

// Transient engine event. Distances are centimetres, impulse is newton seconds.
struct PhysicsNoise {
  Tempest::Vec3 position;
  float volume = 0;
  float radius = 0;
  float impulse = 0;
  ItemMaterial material = MAT_WOOD;
  asset::MaterialGroup surface = asset::MaterialGroup::STONE;
  std::string what;
  uint64_t body = 0;
  Npc* culprit = nullptr;
  };

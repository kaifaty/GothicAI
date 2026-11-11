#pragma once

#include "npc.h"

#include <cstdint>
#include <optional>
#include <string>

// Melee helpers that npc.cpp, npc_damage.cpp and npc_melee_moves.cpp share. Each is defined once:
// meleeBeastFields in npc_damage.cpp, meleeParadeLayer and meleeStaminaIdentity in npc_melee_moves.cpp.
namespace NpcMelee {
std::optional<Pose::CombatLayer> meleeParadeLayer(const Pose& pose, uint64_t now);
std::string                      meleeStaminaIdentity(const Pose& pose, uint64_t now);
std::string                      meleeBeastFields(float mass);
}

using NpcMelee::meleeParadeLayer;
using NpcMelee::meleeStaminaIdentity;
using NpcMelee::meleeBeastFields;

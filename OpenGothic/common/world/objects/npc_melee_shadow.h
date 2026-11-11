#pragma once

#include "npc.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

using Tempest::Vec3;

// Melee log helpers that npc.cpp shares with the melee shadow. Each is defined once, in
// npc_melee_shadow.cpp: writeMeleeShadow owns the one stream, lock and size counter of the log.
namespace NpcMeleeShadow {
std::string meleeShadowWho(const Npc* npc);
void        writeMeleeShadow(const std::string& row);
}

using NpcMeleeShadow::meleeShadowWho;
using NpcMeleeShadow::writeMeleeShadow;

struct Npc::MeleeReach final {
  float advanceCm = std::numeric_limits<float>::infinity(), contactMs = 0, yawDeg = 0, cpuMs = 0;
  float uncoveredCm = std::numeric_limits<float>::infinity();
  float torsoGapCm = std::numeric_limits<float>::infinity();
  };

struct Npc::MeleeShadow final {
  struct Window final {
    const Animation::Sequence* sequence = nullptr;
    float length = 0;
    uint32_t index = 0, first = 0, optimal = 0, last = 0;
    float beginMs = 0, optimalMs = 0, endMs = 0, peakCmS = 0, thresholdCmS = 0;
    bool left = false, normalY = true;
    float eventBoundaryMs = 0, reachCm = 0;
    };
  struct Contact final {
    MeleeBladeContact hit;
    std::string target, bone, result = "shadow";
    float multiplier = 0, stunBonus = 1, backstabMult = 1, chargeMult = 1;
    bool superArmor = false;
    int32_t damage = 0;
    };
  std::vector<Window> windows;
  float reachCpuMs = 0, reachCpuMaxMs = 0;
  uint64_t reachQueries = 0;
  std::optional<Window> window;
  uint64_t id = 0, start = 0, origin = 0, tick = 0;
  float speed = 1;
  std::string attacker, weapon, animation, legacyTarget = "null", legacyResult = "no_optimal_frame";
  const Npc* legacyNpc = nullptr;
  bool legacyHit = false, legacyBlock = false, legacyDodge = false;
  int32_t damage = 0;
  std::vector<Contact> contacts;
  size_t consumed = 0, targetsHit = 0;
  std::vector<uintptr_t> struckVobs; // movable vobs this swing already struck
  bool stopped = false, physicsHits = false;
  float heroDistanceCm = 0;
  struct Magnet final {
    const Npc* target = nullptr;
    Vec3 direction;
    float rawCm = std::numeric_limits<float>::infinity(), requestedCm = 0, offeredCm = 0;
    float combinedCm = 0, estimatedCm = 0, peakCmS = 0, estimatedPeakCmS = 0;
    float yawRequested = 0, yawApplied = 0, peakYawDegS = 0, durationMs = 0;
    float forwardCm = 0, backwardCm = 0, safetyBackoffCm = 0;
    float minTorsoGapCm = std::numeric_limits<float>::infinity(), previewTorsoGapCm = std::numeric_limits<float>::infinity();
    uint64_t safetyFrames = 0, safetyClamps = 0;
    uint64_t contactObservedMs = 0;
    double lastFeedMs = 0;
    const char* contactKind = "none";
    bool eligible = false, feasible = false, contactStopped = false, forwardStopped = false;
    } magnet;
  std::string outcome = "miss";
  struct Diagnostic final {
    const Npc* target = nullptr;
    std::string who = "null";
    uint64_t sample = 0;
    float startDistanceCm = 0;
    Vec3 targetStart, forward, nearestOffset;
    float initialGapCm = 0, minClearanceCm = std::numeric_limits<float>::max(), retreatCm = 0;
    float previewAdvanceCm = 0, previewContactMs = 0, previewCpuMs = 0, previewApproachCm = std::numeric_limits<float>::infinity();
    bool sampled = false, jumpback = false;
    } diagnostic;
  };

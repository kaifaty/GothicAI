#pragma once

#include <cstdint>

// Pure rules for the hero's paired finisher, [GAME] melee_riposte.
namespace MeleeRiposte {
  constexpr uint64_t AlignmentMs    = 400;
  constexpr uint64_t HandoverMs     = 840;
  constexpr float    PairDistanceCm = 165.f;

  struct Situation final {
    bool enabled         = true;
    bool player          = false;
    bool standingOneHand = false;
    bool heroClip        = false;
    bool victimHuman     = false;
    bool victimAlive     = false;
    bool victimDown      = false;
    bool victimStunned   = false;
    bool victimClip      = false;
    float distanceCm     = 0;
    float heightCm       = 0;
    float heroAngleDeg   = 0;
    float victimAngleDeg = 0;
    };

  struct Point final {
    float x = 0;
    float y = 0;
    float z = 0;
    };

  struct DamageMultipliers final {
    float riposte  = 1.f;
    float stun     = 1.f;
    float backstab = 1.f;
    };

  bool  available(const Situation& situation);
  // Forward is the hero's horizontal unit direction after facing the victim.
  Point pairPosition(Point hero, float forwardX, float forwardZ);
  Point alignment(Point from, Point pair, uint64_t elapsedMs);
  uint64_t frameTimeMs(uint32_t normalizedFrame, float fps);
  bool  bladeReady(uint64_t elapsedMs, uint64_t optFrameMs);
  bool  handoverReady(uint64_t elapsedMs);
  float multiplier();
  DamageMultipliers damageMultipliers(float riposte, float stun, float backstab);
  float cost(float attackCost, bool staminaEnabled);
  // The attack covers DEF_HIT_END itself, like charge super armor.
  bool  superArmor(bool active, uint64_t elapsedMs, uint64_t hitEndMs);
  }

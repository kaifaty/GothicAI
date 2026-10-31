#include "meleeimpact.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

Tempest::Vec3 MeleeImpact::impulse(Weapon weapon, const Tempest::Vec3& velocityCmS, float surfaceAngleDeg) {
  const float mass = weapon==Weapon::Fist ? 1.5f : weapon==Weapon::OneHand ? 2.5f : 4.f;
  const float edge = .5f+.5f*std::sin(std::clamp(surfaceAngleDeg,0.f,90.f)*float(M_PI)/180.f);
  auto result = velocityCmS*(mass*.01f*edge);
  const float magnitude = result.length();
  if(magnitude>60.f)
    result *= 60.f/magnitude;
  return result;
  }

float MeleeImpact::skillThresholdScale(int32_t attackerHitChance, int32_t attackerStrength, int32_t defenderHitChance) {
  const float attack  = std::clamp(.7f+.005f*float(std::clamp(attackerHitChance,0,100))+.002f*float(attackerStrength-60),.5f,1.4f);
  const float defence = .8f+.004f*float(std::clamp(defenderHitChance,0,100));
  return 1.2f*defence/attack;
  }

void MeleeImpact::Posture::tick(uint64_t nowMs, uint64_t dtMs) {
  if(nowMs<=lastHitMs+1500)
    return;
  const auto elapsed = std::min(dtMs,nowMs-lastHitMs-1500);
  value = std::max(0.f,value-float(elapsed)*.03f);
  }

// Balance sweeps only: OPENGOTHIC_MELEE_POSTURE_SCALE multiplies the knockdown threshold.
static float postureThresholdScale() {
  static const float scale = [] {
    const char* text = std::getenv("OPENGOTHIC_MELEE_POSTURE_SCALE");
    const float value = text==nullptr ? 1.f : std::strtof(text,nullptr);
    return std::isfinite(value) && value>=.25f && value<=10.f ? value : 1.f;
    }();
  return scale;
  }

bool MeleeImpact::Posture::hit(float impulseNs, float bodyMassKg, uint64_t nowMs, float thresholdScale) {
  const float threshold = 120.f*thresholdScale*postureThresholdScale()*bodyMassKg/75.f;
  value = std::min(nowMs<immuneUntilMs ? 60.f : 100.f,value+100.f*impulseNs/threshold);
  lastHitMs = nowMs;
  return value>=100.f;
  }

void MeleeImpact::Posture::recover(uint64_t nowMs) {
  value = 0;
  lastHitMs = nowMs;
  immuneUntilMs = nowMs+4000;
  }

void MeleeImpact::Posture::reset() {
  *this = Posture();
  }

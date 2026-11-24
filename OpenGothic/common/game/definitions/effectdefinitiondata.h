#pragma once

#include "records/definitions.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace EffectDefinitionData {
inline constexpr std::string_view stockParticleProgram = "53406987b325428b5b16dfa0fd0853e3e8cee7e59ced24a52c0af87c76631e24";
inline constexpr std::string_view stockVisualProgram = "662ce46e81280a587ac35149a207b2393ee63035d156da3cd42baf84be6862e9";
inline constexpr std::size_t stockParticleCount = 528;
inline constexpr std::size_t stockVisualCount = 277;
inline constexpr std::size_t stockKeyCount = 248;

template<class T>
struct Entry {
  uint32_t id;
  std::string name;
  T value;
  };

using Particles = std::vector<Entry<Def::ParticleEffect>>;
struct Visuals {
  std::vector<Entry<Def::EffectBase>> effects;
  std::vector<Entry<Def::ParticleEffectEmitKey>> keys;
  };

std::filesystem::path particleSection(const std::filesystem::path& base, std::string_view program);
std::filesystem::path visualSection(const std::filesystem::path& base, std::string_view program);
Particles readParticles(const std::filesystem::path& base, std::string_view program);
Visuals readVisuals(const std::filesystem::path& base, std::string_view program);
void writeParticles(const std::filesystem::path& base, std::string_view program, const Particles& data);
void writeVisuals(const std::filesystem::path& base, std::string_view program, const Visuals& data);
}

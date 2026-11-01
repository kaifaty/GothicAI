#pragma once

#include "definitiontypes.h"

#include <zenkit/DaedalusVm.hh>
#include <filesystem>
#include <map>
#include <memory>
#include <vector>

namespace ParticleContent {

struct Result {
  std::map<std::string,std::shared_ptr<Def::ParticleEffect>> effects;
  std::vector<std::string> errors;
  };

std::string canonicalName(std::string_view name);
bool hasBase(const zenkit::DaedalusScript& script, std::string_view name);
std::shared_ptr<Def::ParticleEffect> base(zenkit::DaedalusVm& vm, std::string_view name);
std::unique_ptr<zenkit::DaedalusVm> openProgram(const std::filesystem::path& baseRoot);

// Sorted files commit individually. The VM's existing member bindings apply overrides directly
// to the copied definition; ParticleFx builds derived settings through its ordinary constructor.
Result load(zenkit::DaedalusVm& vm, const std::filesystem::path& contentRoot);
Result check(const std::filesystem::path& baseRoot, const std::filesystem::path& contentRoot);

} // namespace ParticleContent

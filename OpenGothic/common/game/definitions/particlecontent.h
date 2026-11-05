#pragma once

#include "definitiontypes.h"

#include "daedalus/vm.h"
#include <filesystem>
#include <map>
#include <memory>
#include <vector>

namespace ParticleContent {

// An own sprite atlas played per particle (cols x rows cells, row by row from the top left); each particle starts
// at its own frame and blends between neighbours (pfxbucket.cpp, pfx.vert).
struct Flipbook {
  int   cols   = 0;
  int   rows   = 0;
  int   frames = 0;
  float fps    = 0;
  bool  yaw    = false;  // upright card turning only about the vertical (a flame seen from above stays standing)
  bool  steady = false;  // the emitter places ppsValue particles once; they never age, fade or move and play the
                         // atlas by the emitter's clock: a flame tongue that stays at its log instead of being reborn
  };

struct Result {
  std::map<std::string,std::shared_ptr<Def::ParticleEffect>> effects;
  std::map<std::string,Flipbook> flipbooks;
  std::vector<std::string> errors;
  };

std::string canonicalName(std::string_view name);
bool hasBase(const daedalus::DaedalusScript& script, std::string_view name);
std::shared_ptr<Def::ParticleEffect> base(daedalus::DaedalusVm& vm, std::string_view name);
std::unique_ptr<daedalus::DaedalusVm> openProgram(const std::filesystem::path& baseRoot);

// Sorted files commit individually. The VM's existing member bindings apply overrides directly
// to the copied definition; ParticleFx builds derived settings through its ordinary constructor.
Result load(daedalus::DaedalusVm& vm, const std::filesystem::path& contentRoot);
Result check(const std::filesystem::path& baseRoot, const std::filesystem::path& contentRoot);

} // namespace ParticleContent

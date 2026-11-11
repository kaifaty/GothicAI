#pragma once

#include "records/definitions.h"

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

// Own motion and look of the particles (pfxmotion.h, pfxbucket.cpp, pfx.vert), in the units of particles.d;
// each field is inherited from an own base and overridden by the entry
struct Motion {
  float curlStrength  = 0;  // m/s^2, the rms acceleration of a divergence-free noise field
  float curlFrequency = 0;  // periods per metre
  float curlSpeed     = 0;  // m/s, the field's drift
  float drag          = 0;  // 1/s, velocity decays towards zero
  float stretch       = 0;  // s of path: a VELO/VELO3D sprite is size + |v|*stretch long
  float minPixels     = 0;  // px: a smaller sprite is drawn this size, its alpha down by the area

  bool empty() const { return curlStrength==0 && drag==0 && stretch==0 && minPixels==0; }
  };

struct Result {
  std::map<std::string,std::shared_ptr<Def::ParticleEffect>> effects;
  std::map<std::string,Flipbook> flipbooks;
  std::map<std::string,Motion> motions;
  std::vector<std::string> errors;
  };

std::string canonicalName(std::string_view name);
bool hasBase(const daedalus::DaedalusScript& script, std::string_view name);
std::shared_ptr<Def::ParticleEffect> base(daedalus::DaedalusVm& vm, std::string_view name);
std::unique_ptr<daedalus::DaedalusVm> openProgram(const std::filesystem::path& baseRoot);

// Sorted files commit individually. Overrides apply to the copied definition's fields;
// ParticleFx builds derived settings through its ordinary constructor.
Result load(daedalus::DaedalusVm& vm, const std::filesystem::path& contentRoot);
Result check(const std::filesystem::path& baseRoot, const std::filesystem::path& contentRoot);

} // namespace ParticleContent

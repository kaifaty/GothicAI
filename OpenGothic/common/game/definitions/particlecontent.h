#pragma once

#include "records/definitions.h"
#include "game/gamehost.h"

#include "daedalus/vm.h"
#include <array>
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
  bool  once   = false;  // the atlas plays once over the particle's life, from its first frame (a burst, a puff)
  };

// Own motion and look of the particles (pfxmotion.h, pfxbucket.cpp, pfx.vert), in the units of particles.d;
// each field is inherited from an own base and overridden by the entry
struct Motion {
  float curlStrength  = 0;  // m/s^2, the rms acceleration of a divergence-free noise field
  float curlFrequency = 0;  // periods per metre
  float curlSpeed     = 0;  // m/s, the field's drift
  float drag          = 0;  // 1/s, velocity decays towards the air's (zero, with "wind" the wind's)
  float stretch       = 0;  // s of path: a VELO/VELO3D sprite is size + |v|*stretch long
  float minPixels     = 0;  // px: a smaller sprite is drawn this size, its alpha down by the area
  // "hdr" of an additive effect (main.frag mainEmissive): how the night's gain of the emission (up to x6) goes over
  // a texel's own brightness, its brightest channel in the scene's white at noon. Up to hdrFrom a texel takes the
  // share hdrEdge of the gain, so the edges of a flame keep their orange under the tone curve; from hdrTo all of
  // it, so the core goes to white. hdrTo 0: off, the whole sprite takes the whole gain
  float hdrFrom       = 0;
  float hdrTo         = 0;
  float hdrEdge       = 1;
  // "haze" (main.frag mainHaze): the sprite has no colour of its own and shifts the finished frame behind it by a
  // rising noise, the shimmer of hot air. hazeShift the largest shift in cm at the sprite's centre and full alpha,
  // hazeCell the size of a noise cell in cm; hazeShift 0: off
  float hazeShift     = 0;
  float hazeCell      = 0;
  // "lean" of a steady flame card (pfxbucket.cpp, pfx.vert): the card stands on its lower edge and leans with the
  // air that passes its emitter (minus the emitter's own motion). leanRise is the flame gas's rise in m/s: the
  // tangent of the lean is the air's speed over it; leanRise 0: off. leanMaxDeg caps the angle, leanResponse is the
  // time in s the flame takes to answer a change of the air
  float leanRise      = 0;
  float leanMaxDeg    = 70;
  float leanResponse  = 0.12f;
  float leanAlong     = 0.75f; // the share of its height the card keeps when seen along the flame
  float leanGrow      = 0;     // seconds a new emitter's card takes to grow from leanGrowFrom of its size; 0: off
  float leanGrowFrom  = 0.2f;
  // "inherit": the share 0..1 of its emitter's motion a living particle takes, so the tail behind a carried flame
  // is short: at 0.75 a tongue trails at a quarter of the carrier's speed
  float inherit       = 0;
  // "wind": the share of the world's wind (physics/worldwind.h; none under a roof, less behind a wall) the effect
  // feels: a leaning card leans with it, a particle with drag is carried by it
  float wind          = 0;
  // "plume": the emitter stands plumeHeight cm over a fire whose gas rises at plumeRise m/s. In moving air its
  // particles are born that far along the leaning flame and with the wind's velocity: smoke leaves the flame's tip
  float plumeHeight   = 0;
  float plumeRise     = 0;
  float plumeWind     = 0;  // the flame's own "wind" share: its lean differs from the smoke's drift
  // "light": a spell effect or an animation event's effect (graphics/effect) of this particle effect brings a point
  // light of lightRange cm and the colour lightColor (0..255), a spell's instead of the game's preset. lightKeys are
  // the range's scale over the effect's life at lightFps keys a second (a fire that spreads, holds and dies down);
  // none: the full range. lightColors are its colours (0..255) at the same rate in place of lightColor. The keys play
  // once and hold the last, with lightLoop they go round. lightAt is its place in cm in the axes of the effect's node
  // (a forge's bowl over its bone). 0: off
  float lightRange    = 0;
  float lightColor[3] = {};
  std::vector<float> lightKeys;
  std::vector<std::array<float,3>> lightColors;
  float lightFps      = 0;
  float lightAt[3]    = {};
  bool  lightLoop     = false;

  bool empty() const { return curlStrength==0 && drag==0 && stretch==0 && minPixels==0 && hdrTo==0 && hazeShift==0 && leanRise==0 && inherit==0 && wind==0 && plumeHeight==0 && lightRange==0; }
  };

struct Result {
  std::map<std::string,std::shared_ptr<Def::ParticleEffect>> effects;
  std::map<std::string,Flipbook> flipbooks;
  std::map<std::string,Motion> motions;
  // "with": own effects every emitter of this one brings along at its place (flame + tongues + sparks + smoke)
  std::map<std::string,std::vector<std::string>> with;
  // "land": the own effect put where a particle of this one meets the world's static geometry (a drop of the fire
  // rain); the particle stops there
  std::map<std::string,std::string> land;
  // "node": an animation event's effect of this one stands on this node of its object instead of the event's
  // (World::animationPfx), upper case as a model's nodes; not inherited
  std::map<std::string,std::string> nodes;
  std::vector<std::string> errors;
  };

GameHost::ParticleNameCheck nameChecker();
std::string canonicalName(std::string_view name);
bool hasBase(const daedalus::DaedalusScript& script, std::string_view name);
std::shared_ptr<Def::ParticleEffect> base(daedalus::DaedalusVm& vm, std::string_view name);
std::unique_ptr<daedalus::DaedalusVm> openProgram(const std::filesystem::path& baseRoot);

// Sorted files commit individually. Overrides apply to the copied definition's fields;
// ParticleFx builds derived settings through its ordinary constructor.
Result load(daedalus::DaedalusVm& vm, const std::filesystem::path& contentRoot);
Result check(const std::filesystem::path& baseRoot, const std::filesystem::path& contentRoot);

} // namespace ParticleContent

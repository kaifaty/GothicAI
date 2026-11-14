// The island program's hero (engine layers, step 8.6: reports/engine/2026-11-13-empty-island/README.md) on the
// island's collision, as engine-island walks it: the ground that island-data wrote, the props placed by
// island::World, the capsule of island::Hero stepped at 60 steps a second. No window, no device, no Gothic, no base;
// the test links as the program does, without OpenGothic.
//
//   gothic-island-program-test
//
// It checks, by the numbers of islandbody.h: standing on the ground, the walking and running speed on a level
// meadow, the jump's height and landing, walking up a slope under the limit and not up one over it, stopping in the
// sea at the wading depth, and a boulder that stops the capsule.

#include "islandbody.h"
#include "islandhero.h"
#include "islandservices.h"
#include "islandworld.h"

#include "resources.h"
#include "worldchunksruntime.h"
#include "physics/dynamicworld.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using Tempest::Vec3;

namespace {
size_t assertions = 0;

void require(bool condition, const std::string& message) {
  ++assertions;
  if(!condition)
    throw std::runtime_error(message);
  }

#if defined(__GNUC__)
__attribute__((format(printf, 1, 2)))
#endif
std::string text(const char* format, ...) {
  char buffer[512];
  va_list args;
  va_start(args, format);
  std::vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  return buffer;
  }

constexpr float Pi = 3.14159265358979f;

float radians(float degrees) {
  return degrees*Pi/180.f;
  }

float horizontal(const Vec3& a, const Vec3& b) {
  return std::hypot(a.x-b.x, a.z-b.z);
  }

// The island stepped as engine-island steps it: 60 steps a second of 16 or 17 ms, the hero, then the physics.
struct Sim {
  island::World& world;
  island::Hero&  hero;
  uint64_t       steps = 0;

  // `ms` milliseconds of `control`; a jump only on the first step. `each` sees the hero after every step.
  void run(uint64_t ms, island::Control control, const std::function<void()>& each = {}) {
    const uint64_t end = stepEnd(steps)+ms;
    bool first = true;
    while(stepEnd(steps)<end) {
      const uint64_t dt = stepEnd(steps+1)-stepEnd(steps);
      island::Control c = control;
      c.jump = control.jump && first;
      first  = false;
      hero.tick(c, dt);
      world.physics().tick(dt);
      ++steps;
      if(each)
        each();
      }
    }

  static uint64_t stepEnd(uint64_t step) {
    return step*1000/60;
    }

  // Puts the hero at (x, z) metres facing `yaw`, and lets him settle for half a second.
  void place(float x, float z, float yaw) {
    const float ground = world.land(x*100.f, z*100.f);
    require(std::isfinite(ground), text("the ground must cover (%.1f, %.1f)", double(x), double(z)));
    hero.teleport(Vec3(x*100.f, ground+5.f, z*100.f), yaw);
    run(500, {});
    require(hero.onGround(), text("the hero must stand at (%.1f, %.1f)", double(x), double(z)));
    }
  };

// The ground's slope at (x, z) in degrees, over half a metre each way, and the direction it rises in (radians).
float slopeAt(const island::World& world, float x, float z, float* uphill = nullptr) {
  const float e  = 0.5f;
  const float gx = (world.land((x+e)*100.f, z*100.f)-world.land((x-e)*100.f, z*100.f))/(200.f*e);
  const float gz = (world.land(x*100.f, (z+e)*100.f)-world.land(x*100.f, (z-e)*100.f))/(200.f*e);
  if(uphill!=nullptr)
    *uphill = std::atan2(gz, gx);
  return std::atan(std::hypot(gx, gz))*180.f/Pi;
  }

float nearestProp(const island::World& world, float x, float z) {
  float best = std::numeric_limits<float>::max();
  for(const auto& p:world.props())
    best = std::min(best, std::hypot(p.position.x*0.01f-x, p.position.z*0.01f-z)-p.foot);
  return best;
  }

// A level stretch of the meadow: `length` metres from (x, z) along `yaw`, never steeper than `limit`, away from the
// props and the water.
bool levelStretch(const island::World& world, float length, float limit, float& x, float& z, float& yaw) {
  for(int ring=30; ring<=90; ring+=10)
    for(int a=0; a<360; a+=10)
      for(int d=0; d<360; d+=45) {
        x   = float(ring)*std::cos(radians(float(a)));
        z   = float(ring)*std::sin(radians(float(a)));
        yaw = float(d);
        bool fits = true;
        for(float s=0; s<=length && fits; s+=1.f) {
          const float px = x+std::cos(radians(yaw))*s, pz = z+std::sin(radians(yaw))*s;
          fits = world.land(px*100.f, pz*100.f)>100.f && slopeAt(world, px, pz)<limit && nearestProp(world, px, pz)>3.f;
          }
        if(fits)
          return true;
        }
  return false;
  }

void checkStanding(Sim& sim, float x, float z) {
  sim.place(x, z, 0);
  const Vec3 from = sim.hero.position();
  float worst = 0;
  sim.run(3000, {}, [&] {
    const float ground = sim.world.land(sim.hero.position().x, sim.hero.position().z);
    worst = std::max(worst, std::abs(sim.hero.position().y-ground));
    require(sim.hero.onGround(), "the hero must not leave the ground while he stands");
    });
  const float drift = horizontal(sim.hero.position(), from);
  std::cout << text("stand: feet within %.2f cm of the ground for 3 s, drifted %.2f cm\n", double(worst), double(drift));
  require(worst<=2.f, text("the feet must stay within 2 cm of the ground: %.2f cm", double(worst)));
  require(drift<=1.f, text("a standing hero must not slide: %.2f cm", double(drift)));
  }

void checkSpeed(Sim& sim, float x, float z, float yaw, bool run) {
  sim.place(x, z, yaw);
  island::Control c;
  c.forward = 1;
  c.run     = run;
  sim.run(500, c); // up to speed: the first step starts from rest
  const Vec3 from = sim.hero.position();
  sim.run(5000, c, [&] { require(sim.hero.onGround(), "the hero must keep to the meadow"); });
  const float speed  = horizontal(sim.hero.position(), from)*0.01f/5.f;
  const float wanted = island::Hero::speed(run);
  std::cout << text("%s: %.3f m/s over 5 s on the meadow, %.3f m/s wanted\n", run ? "run" : "walk", double(speed),
                    double(wanted));
  require(std::abs(speed-wanted)<=wanted*0.05f,
          text("the %s must be %.2f m/s within 5 %%: %.3f m/s", run ? "run" : "walk", double(wanted), double(speed)));
  }

void checkJump(Sim& sim, float x, float z) {
  sim.place(x, z, 0);
  const float start = sim.hero.position().y;
  const auto  jumps = sim.hero.jumps(), landings = sim.hero.landings();
  island::Control c;
  c.jump = true;
  float    apex = 0, air = 0;
  bool     left = false, back = false;
  uint64_t t = 0;
  sim.run(2000, c, [&] {
    t += 1;
    apex = std::max(apex, (sim.hero.position().y-start)*0.01f);
    if(!sim.hero.onGround()) {
      left = true;
      if(!back)
        air = float(t)/60.f;
      }
    else if(left) {
      back = true;
      }
    });
  const float flight = 2.f*std::sqrt(2.f*island::JumpHeight/island::Gravity);
  std::cout << text("jump: apex %.3f m (%.2f wanted), %.3f s in the air (%.3f), %u jump, %u landing\n", double(apex),
                    double(island::JumpHeight), double(air), double(flight), sim.hero.jumps()-jumps,
                    sim.hero.landings()-landings);
  require(sim.hero.jumps()==jumps+1, "a press of the jump must begin one jump");
  require(apex>=island::JumpHeight*0.95f && apex<=island::JumpHeight*1.05f,
          text("the jump's apex must be %.2f m within 5 %%: %.3f m", double(island::JumpHeight), double(apex)));
  require(back && sim.hero.onGround() && !sim.hero.jumping(), "the hero must land and stand again");
  require(sim.hero.landings()==landings+1, "the landing must be counted once");
  require(std::abs(air-flight)<=0.05f, text("the flight must last %.2f s: %.3f s", double(flight), double(air)));
  require(std::abs(sim.hero.position().y-start)<=2.f, "he lands where he jumped from");
  }

// Walks up from `from` along `uphill` (radians) for `ms`; returns how high he got over his start, metres.
float climb(Sim& sim, float x, float z, float uphill, uint64_t ms, bool run, float& progress) {
  sim.place(x, z, uphill*180.f/Pi);
  const Vec3 from = sim.hero.position();
  float top = from.y;
  island::Control c;
  c.forward = 1;
  c.run     = run;
  sim.run(ms, c, [&] { top = std::max(top, sim.hero.position().y); });
  progress = horizontal(sim.hero.position(), from)*0.01f;
  return (top-from.y)*0.01f;
  }

// Under the limit: a run of ground of 20-30 degrees, 5 m up; he walks up it.
void checkGentleSlope(Sim& sim) {
  for(int ring=10; ring<=80; ring+=2)
    for(int a=0; a<360; a+=5) {
      const auto& top = sim.world.summit();
      const float x = top.x*0.01f+float(ring)*std::cos(radians(float(a)));
      const float z = top.z*0.01f+float(ring)*std::sin(radians(float(a)));
      float uphill = 0;
      const float s0 = slopeAt(sim.world, x, z, &uphill);
      if(s0<20.f || s0>30.f || nearestProp(sim.world, x, z)<4.f)
        continue;
      bool fits = true;
      for(float s=-1.f; s<=5.f && fits; s+=0.5f) {
        const float g = slopeAt(sim.world, x+std::cos(uphill)*s, z+std::sin(uphill)*s);
        fits = g>=15.f && g<=30.f;
        }
      if(!fits)
        continue;
      float progress = 0;
      const float rise = climb(sim, x, z, uphill, 3000, false, progress);
      std::cout << text("slope %.1f degrees at (%.0f, %.0f): walked %.2f m up it, %.2f m higher\n", double(s0),
                        double(x), double(z), double(progress), double(rise));
      require(progress>=0.8f*island::WalkSpeed*3.f*std::cos(radians(30.f)),
              text("ground of %.1f degrees must be walked up: %.2f m in 3 s", double(s0), double(progress)));
      require(rise>=1.f, text("walking up %.1f degrees he must rise: %.2f m", double(s0), double(rise)));

      // a jump on the run up the slope lands as soon as the flight is over
      sim.place(x, z, uphill*180.f/Pi);
      island::Control c;
      c.forward = 1;
      c.run     = true;
      sim.run(300, c);
      c.jump = true;
      const auto landings = sim.hero.landings();
      float air = 0;
      sim.run(1500, c, [&] {
        if(sim.hero.landings()==landings)
          air += 1.f/60.f;
        });
      const float flight = 2.f*std::sqrt(2.f*island::JumpHeight/island::Gravity);
      std::cout << text("running jump up %.1f degrees: %.3f s in the air (%.3f on level ground)\n", double(s0),
                        double(air), double(flight));
      require(sim.hero.landings()==landings+1 && sim.hero.onGround(), "a jump up the slope must land");
      require(air<=flight, text("up the slope the flight must not outlast a level one: %.3f s", double(air)));
      return;
      }
  throw std::runtime_error("the island must have ground of 20-30 degrees, 5 m up");
  }

// Over the limit: a steep triangle of the ground with its uphill side steep for 3 m more; he does not run up it.
void checkSteepSlope(Sim& sim, const asset::WorldMesh& mesh) {
  struct Steep { float slope, x, z; };
  std::vector<Steep> steep;
  const auto& p = mesh.polygons;
  for(size_t t=0; t<p.material_indices.size(); ++t) {
    const auto& a = mesh.vertices[p.vertex_indices[t*3+0]];
    const auto& b = mesh.vertices[p.vertex_indices[t*3+1]];
    const auto& c = mesh.vertices[p.vertex_indices[t*3+2]];
    const float ux = b.x-a.x, uy = b.y-a.y, uz = b.z-a.z, vx = c.x-a.x, vy = c.y-a.y, vz = c.z-a.z;
    const float nx = uy*vz-uz*vy, ny = uz*vx-ux*vz, nz = ux*vy-uy*vx;
    const float l  = std::sqrt(nx*nx+ny*ny+nz*nz);
    if(l<=0 || std::min({a.y, b.y, c.y})<100.f)
      continue;
    const float slope = std::acos(std::min(1.f, std::abs(ny)/l))*180.f/Pi;
    if(slope>island::WalkSlope+8.f)
      steep.push_back({slope, (a.x+b.x+c.x)/300.f, (a.z+b.z+c.z)/300.f});
    }
  std::sort(steep.begin(), steep.end(), [](const Steep& l, const Steep& r) { return l.slope>r.slope; });
  require(!steep.empty(), "the island must have ground steeper than the limit");
  for(const auto& s:steep) {
    float uphill = 0;
    slopeAt(sim.world, s.x, s.z, &uphill);
    // the foot of the steep: back down until the ground is walkable
    float back = 0;
    while(back<6.f && slopeAt(sim.world, s.x-std::cos(uphill)*back, s.z-std::sin(uphill)*back)>island::WalkSlope)
      back += 0.25f;
    bool fits = back<6.f;
    for(float d=0; d<=3.f && fits; d+=0.25f)
      fits = slopeAt(sim.world, s.x+std::cos(uphill)*d, s.z+std::sin(uphill)*d)>island::WalkSlope+5.f;
    const float fx = s.x-std::cos(uphill)*(back+1.5f), fz = s.z-std::sin(uphill)*(back+1.5f);
    if(!fits || slopeAt(sim.world, fx, fz)>island::WalkSlope-5.f || nearestProp(sim.world, fx, fz)<4.f)
      continue;
    const float band = (sim.world.land((s.x+std::cos(uphill)*3.f)*100.f, (s.z+std::sin(uphill)*3.f)*100.f)-
                        sim.world.land(fx*100.f, fz*100.f))*0.01f;
    float progress = 0;
    const float rise = climb(sim, fx, fz, uphill, 4000, true, progress);
    std::cout << text("steep: a triangle of %.1f degrees at (%.0f, %.0f), the ground %.2f m higher 3 m up it; running "
                      "at it for 4 s he rose %.2f m\n", double(s.slope), double(s.x), double(s.z), double(band),
                      double(rise));
    require(band>=2.f, "the steep ground must rise 2 m and more");
    require(rise<=0.5f*band, text("ground over %.0f degrees must not be run up: %.2f m of %.2f m",
                                  double(island::WalkSlope), double(rise), double(band)));
    return;
    }
  throw std::runtime_error("the island must have a steep band with walkable ground at its foot");
  }

// Out to sea from the demo's start: he wades in and stops at his hips.
void checkSea(Sim& sim) {
  const auto& start = sim.world.start();
  sim.place(start.feet.x*0.01f, start.feet.z*0.01f, start.yaw+180.f);
  island::Control c;
  c.forward = 1;
  sim.run(40000, c);
  Vec3  last  = sim.hero.position();
  float moved = 0;
  sim.run(3000, c, [&] {
    moved += horizontal(sim.hero.position(), last)*0.01f;
    last   = sim.hero.position();
    });
  // the water's surface from the sky down, the ground under it by a ray onto the landscape
  const auto& at     = sim.hero.position();
  const float ground = sim.world.land(at.x, at.z);
  const auto  water  = sim.world.physics().navigationRay(Vec3(at.x, 3000, at.z), Vec3(at.x, ground-1.f, at.z), nullptr);
  const bool  sea    = water.hasCol && water.kind==DynamicWorld::NavigationHit::Water;
  const float depth  = sea ? (water.point.y-ground)*0.01f : 0.f;
  std::cout << text("sea: stopped %.2f m deep (the hero says %.2f), %.1f m from the start, %.3f m in the last 3 s\n",
                    double(depth), double(sim.hero.waterDepth()), double(horizontal(at, start.feet)*0.01f),
                    double(moved));
  require(sea, "the hero must be in the sea");
  require(depth>=island::WadeDepth-0.05f && depth<=island::WadeDepth+0.02f,
          text("he must stop %.2f m deep: %.3f m", double(island::WadeDepth), double(depth)));
  require(moved<=0.05f, text("he must not go on into the sea: %.3f m", double(moved)));
  require(sim.hero.onGround(), "he stands on the sea bottom");
  }

// The largest stone stops him: walking at its middle for 5 s, he stays on his side of it.
void checkBoulder(Sim& sim) {
  const island::Placed* rock = nullptr;
  for(const auto& p:sim.world.props())
    if(p.visual=="ISLAND_ROCK_3.3DS")
      rock = &p;
  require(rock!=nullptr, "the island must have its largest stone placed");
  const float cx = rock->position.x*0.01f, cz = rock->position.z*0.01f;
  for(int a=0; a<360; a+=30) {
    const float dx = std::cos(radians(float(a))), dz = std::sin(radians(float(a)));
    const float x = cx+dx*4.f, z = cz+dz*4.f;
    bool fits = true;
    for(float s=1.6f; s<=4.f && fits; s+=0.4f)
      fits = slopeAt(sim.world, cx+dx*s, cz+dz*s)<15.f;
    if(!fits)
      continue;
    sim.place(x, z, float(a)+180.f);
    island::Control c;
    c.forward = 1;
    float closest = 1e9f;
    sim.run(5000, c, [&] {
      closest = std::min(closest, std::hypot(sim.hero.position().x*0.01f-cx, sim.hero.position().z*0.01f-cz));
      });
    const float side = (sim.hero.position().x*0.01f-cx)*dx+(sim.hero.position().z*0.01f-cz)*dz;
    std::cout << text("boulder: walking at its middle from 4 m he came within %.2f m of it and stayed %.2f m on his "
                      "side\n", double(closest), double(side));
    require(closest>=0.6f, text("the stone must stop the capsule: %.2f m from its middle", double(closest)));
    require(side>0.5f, text("he must stay on his side of the stone: %.2f m", double(side)));
    return;
    }
  throw std::runtime_error("the largest stone must have level ground around it");
  }
}

int main(int argc, char**) {
  try {
    if(argc!=1)
      throw std::runtime_error("usage: gothic-island-program-test");
    const island::DataDir data{ISLAND_DATA_DIR};
    island::useIslandFolders(data);
    island::Arguments arguments(data.root);
    Resources         resources;           // no device: the meshes for their collision only
    island::Options   options(data.root);
    island::Services  services;

    island::World world(data);
    require(Resources::selectedBaseFingerprint().empty(), "no base is selected");
    const auto& start = world.start();
    island::Hero hero(world.physics(), start.feet+Vec3(0, 5, 0), start.yaw);
    services.physics = &world.physics();
    services.target  = &hero;
    Sim sim{world, hero};

    float x = 0, z = 0, yaw = 0;
    require(levelStretch(world, 20.f, 4.f, x, z, yaw), "the meadow must have a level stretch of 20 m");
    std::cout << text("meadow: (%.0f, %.0f) along %.0f degrees\n", double(x), double(z), double(yaw));
    checkStanding(sim, x, z);
    checkSpeed(sim, x, z, yaw, false);
    checkSpeed(sim, x, z, yaw, true);
    checkJump(sim, x, z);

    checkGentleSlope(sim);
    checkSteepSlope(sim, readWorldChunks(data.world()));
    checkSea(sim);
    checkBoulder(sim);
    services.target = nullptr;
    std::cout << "{\"test\":\"island_program\",\"passed\":true,\"assertions\":" << assertions << "}\n";
    return 0;
    }
  catch(const std::exception& error) {
    std::cerr << "island program test failed: " << error.what() << '\n';
    return 1;
    }
  }

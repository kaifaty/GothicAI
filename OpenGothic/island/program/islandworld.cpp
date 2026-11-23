#include "islandworld.h"

#include "islandbody.h"
#include "islandprops.h"
#include "islandterrain.h"

#include "resources.h"
#include "graphics/mesh/protomesh.h"
#include "physics/dynamicworld.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <stdexcept>

using namespace Tempest;

namespace island {

namespace {
constexpr float Pi = 3.14159265358979f;

float radians(float degrees) {
  return degrees*Pi/180.f;
  }

// The stream's centre line on land: from the spring to the first point in the sea.
size_t mouthOf(const std::vector<StreamPoint>& stream) {
  size_t mouth = 0;
  while(mouth+1<stream.size() && terrain().shore(stream[mouth].x, stream[mouth].z)>0)
    ++mouth;
  return mouth;
  }

float toStream(float x, float z) {
  float best = std::numeric_limits<float>::max();
  for(const auto& p:terrain().stream())
    best = std::min(best, std::hypot(p.x-x, p.z-z));
  return best;
  }

// The point on the ray from (x0, z0) along `angle` where the ground is `inland` metres from the water.
bool shorePoint(float x0, float z0, float angle, float inland, float& x, float& z) {
  const float dx = std::cos(angle), dz = std::sin(angle);
  for(float r=0; r<FieldHalf; r+=0.25f) {
    x = x0+dx*r;
    z = z0+dz*r;
    if(terrain().shore(x, z)<=inland)
      return r>0;
    }
  return false;
  }

int cellOf(float metres) {
  return int(std::floor(metres*100.f/float(ChunkId::Size)));
  }
}

void useIslandFolders(const DataDir& data) {
#if defined(_WIN32)
  _putenv_s("OPENGOTHIC_CONTENT",  data.content().string().c_str());
  _putenv_s("OPENGOTHIC_TEXTURES", data.textures().string().c_str());
#else
  setenv("OPENGOTHIC_CONTENT",  data.content().string().c_str(),  1);
  setenv("OPENGOTHIC_TEXTURES", data.textures().string().c_str(), 1);
#endif
  }

World::World(const DataDir& data) {
  if(!std::filesystem::is_regular_file(data.world()/"world.json"))
    throw std::runtime_error("no island at "+data.root.string()+" (build the island-data target)");
  mesh = readWorldChunks(data.world(), {}, &table);
  if(table.entries.empty())
    throw std::runtime_error("the island at "+data.root.string()+" has no chunks");
  // the whole ground at once: collision streaming by chunks is not the island's (the plan's "out of scope")
  dynamic = std::make_unique<DynamicWorld>(nullptr, mesh);

  top = Vec3(0, -1e9f, 0);
  for(const auto& v:mesh.vertices)
    if(v.y>top.y)
      top = Vec3(v.x, v.y, v.z);

  placeStart();
  place();
  }

World::~World() = default;

asset::WorldMesh World::takeMesh() {
  return std::move(mesh);
  }

float World::ground(float x, float z) const {
  const auto hit = dynamic->rayStatic(Vec3(x, 20000, z), Vec3(x, -5000, z));
  return hit.hasCol ? hit.v.y : std::numeric_limits<float>::quiet_NaN();
  }

float World::land(float x, float z) const {
  const auto hit = dynamic->landRay(Vec3(x, 20000, z), 25000);
  return hit.hasCol ? hit.v.y : std::numeric_limits<float>::quiet_NaN();
  }

// The start of the demo: the first ray out of the summit, by fixed order, on which a man standing on the dry beach and
// running up to the summit crosses a chunk border 6-18 m ahead, over gentle ground away from the stream, and has
// room to turn right after 20 m.
void World::placeStart() {
  const float sx = top.x*0.01f, sz = top.z*0.01f;
  for(int k=0; k<72; ++k) {
    const float out = radians(float(k)*5.f);
    float x = 0, z = 0;
    if(!shorePoint(sx, sz, out, 12.f, x, z))
      continue;
    const float hx = -std::cos(out), hz = -std::sin(out); // up to the summit
    float cross = -1;
    bool  clear = true;
    float last  = land(x*100.f, z*100.f);
    for(float s=0.5f; s<=45.f && clear; s+=0.5f) {
      const float px = x+hx*s, pz = z+hz*s;
      if(cross<0 && (cellOf(px)!=cellOf(x) || cellOf(pz)!=cellOf(z)))
        cross = s;
      const float h = land(px*100.f, pz*100.f);
      clear = std::isfinite(h) && std::abs(h-last)<=50.f*std::tan(radians(18.f)) && toStream(px, pz)>15.f;
      last  = h;
      }
    // the turn: right of the way, 15 m more from 20 m on
    const float rx = x+hx*20.f, rz = z+hz*20.f;
    for(float s=1.f; s<=15.f && clear; s+=1.f)
      clear = toStream(rx+hz*s, rz-hx*s)>10.f && terrain().shore(rx+hz*s, rz-hx*s)>8.f;
    if(!clear || cross<6.f || cross>18.f)
      continue;
    begin.feet = Vec3(x*100.f, land(x*100.f, z*100.f), z*100.f);
    begin.yaw  = std::atan2(hz, hx)*180.f/Pi;
    return;
    }
  throw std::runtime_error("the island has no start for the demo: no ray from the summit fits");
  }

void World::addProp(const std::string& name, float x, float z, float turn, float foot) {
  // on a slope it stands on the lowest ground under it: no air under its downhill side
  float low = land(x*100.f, z*100.f);
  for(int i=0; i<8; ++i) {
    const float a = float(i)*Pi*0.25f;
    low = std::min(low, land((x+foot*std::cos(a))*100.f, (z+foot*std::sin(a))*100.f));
    }
  Placed p;
  p.visual   = name+".3DS";
  p.position = Vec3(x*100.f, low, z*100.f);
  p.foot     = foot;
  p.at       = Matrix4x4::mkIdentity();
  p.at.translate(p.position);
  p.at.rotateOY(turn);

  const auto* proto = Resources::loadMesh(p.visual);
  if(proto==nullptr)
    throw std::runtime_error("the island's prop is not found: "+p.visual);
  PhysicMesh body(*proto, *dynamic, false);
  if(body.isEmpty())
    throw std::runtime_error("the island's prop has no collision: "+p.visual);
  body.setObjMatrix(p.at);
  bodies.push_back(std::move(body));
  placed.push_back(std::move(p));
  }

void World::place() {
  const auto& stream = terrain().stream();
  const size_t mouth = mouthOf(stream);
  if(mouth<60)
    throw std::runtime_error("the island's stream runs less than 60 m over land");

  // The shed: on the most level ground beside the stream's lower half, on the meadow, along the stream.
  struct Best { float spread = 1e9f, x = 0, z = 0, turn = 0; } shed;
  for(size_t up=25; up<=std::min<size_t>(70, mouth-1); up+=3) {
    const auto& p = stream[mouth-up];
    const auto& q = stream[mouth-up-1];
    const float tx = q.x-p.x, tz = q.z-p.z, tl = std::hypot(tx, tz);
    for(float aside:{-15.f, -12.f, -9.f, 9.f, 12.f, 15.f}) {
      const float x = p.x+tz/tl*aside, z = p.z-tx/tl*aside;
      if(terrain().shore(x, z)<25.f)
        continue;
      Best c;
      c.x    = x;
      c.z    = z;
      c.turn = std::atan2(tz, tx)*180.f/Pi;
      auto at = Matrix4x4::mkIdentity();
      at.translate(x*100.f, 0, z*100.f);
      at.rotateOY(c.turn);
      float lo = 1e9f, hi = -1e9f;
      for(float px:{-150.f, 150.f})
        for(float pz:{-110.f, 110.f}) {
          Vec3 post = {px, 0, pz};
          at.project(post);
          const float h = land(post.x, post.z);
          lo = std::min(lo, h);
          hi = std::max(hi, h);
          }
      c.spread = hi-lo;
      if(lo>150.f && c.spread<shed.spread)
        shed = c;
      }
    }
  if(shed.spread>40.f)
    throw std::runtime_error("the island has no level place for the shed beside the stream");
  addProp("ISLAND_SHED", shed.x, shed.z, shed.turn, 1.9f);
  {
    // two lamps under the front beam, 2 m up, near the front posts
    const auto& at = placed.back().at;
    for(float px:{-120.f, 120.f}) {
      Vec3 lamp = {px, 200.f, 95.f};
      at.project(lamp);
      lampAt.push_back(lamp);
      }
  }

  // Stones on the meadow and at the beach: by fixed directions out of the island's middle, `inland` metres from the
  // water, turned away from the stream, the shed and the demo's way.
  struct Stone { const char* name; float angle, inland, turn, foot; };
  const Stone stones[] = {
    {"ISLAND_ROCK_3", 200.f, 42.f, 250.f, 0.9f}, {"ISLAND_ROCK_1", 235.f, 38.f,  20.f, 0.6f},
    {"ISLAND_ROCK_2", 300.f, 45.f, 110.f, 0.8f}, {"ISLAND_ROCK_1", 120.f, 40.f, 160.f, 0.6f},
    {"ISLAND_ROCK_2", 150.f, 10.f, 320.f, 0.8f}, {"ISLAND_ROCK_4", 170.f, 11.f, 200.f, 0.25f},
    {"ISLAND_ROCK_4", 262.f,  9.f,  70.f, 0.25f}, {"ISLAND_ROCK_1", 330.f, 10.f, 300.f, 0.6f},
    };
  const float hx = std::cos(radians(begin.yaw)), hz = std::sin(radians(begin.yaw));
  const float bx = begin.feet.x*0.01f, bz = begin.feet.z*0.01f;
  auto offWay = [&](float x, float z) {
    for(float s=0; s<=45.f; s+=1.f)
      if(std::hypot(bx+hx*s-x, bz+hz*s-z)<6.f)
        return false;
    return true;
    };
  for(const auto& stone:stones) {
    for(int attempt=0; attempt<12; ++attempt) {
      float x = 0, z = 0;
      if(!shorePoint(0, 0, radians(stone.angle+float(attempt)*7.f), stone.inland, x, z))
        continue;
      if(toStream(x, z)<8.f || std::hypot(x-shed.x, z-shed.z)<10.f || !offWay(x, z))
        continue;
      addProp(stone.name, x, z, stone.turn, stone.foot);
      break;
      }
    }
  }

}

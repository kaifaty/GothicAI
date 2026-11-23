#include "islandprops.h"

#include "islandnoise.h"
#include "islandtextures.h"

#include "gothicglb.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <map>
#include <stdexcept>

namespace island {

namespace {

struct V3 {
  float x = 0, y = 0, z = 0;
  };

V3    operator+(const V3& a, const V3& b) { return {a.x+b.x, a.y+b.y, a.z+b.z}; }
V3    operator-(const V3& a, const V3& b) { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
V3    operator*(const V3& a, float k)     { return {a.x*k, a.y*k, a.z*k}; }
float dot(const V3& a, const V3& b)       { return a.x*b.x + a.y*b.y + a.z*b.z; }
V3    cross(const V3& a, const V3& b)     { return {a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x}; }
V3    unit(const V3& a) {
  const float l = std::sqrt(dot(a, a));
  return l>1e-12f ? a*(1.f/l) : V3{0, 1, 0};
  }

// glTF space: metres, y up, counter-clockwise front.
struct Shape {
  std::vector<float>    position, normal, uv;
  std::vector<uint32_t> index;

  uint32_t add(const V3& p, const V3& n, float u, float v) {
    position.insert(position.end(), {p.x, p.y, p.z});
    normal.insert(normal.end(), {n.x, n.y, n.z});
    uv.insert(uv.end(), {u, v});
    return uint32_t(position.size()/3-1);
    }
  };

// --- stones ---------------------------------------------------------------------------------------------------------

struct Sphere {
  std::vector<V3>                      points;
  std::vector<std::array<uint32_t, 3>> faces;
  };

// An icosahedron, each face split in four `levels` times, the points pushed to the unit sphere.
Sphere icosphere(int levels) {
  const float t = (1.f+std::sqrt(5.f))*0.5f;
  Sphere s;
  for(const V3& p:{V3{-1, t, 0}, V3{1, t, 0}, V3{-1, -t, 0}, V3{1, -t, 0}, V3{0, -1, t}, V3{0, 1, t},
                   V3{0, -1, -t}, V3{0, 1, -t}, V3{t, 0, -1}, V3{t, 0, 1}, V3{-t, 0, -1}, V3{-t, 0, 1}})
    s.points.push_back(unit(p));
  s.faces = {{0, 11, 5}, {0, 5, 1},  {0, 1, 7},   {0, 7, 10}, {0, 10, 11}, {1, 5, 9}, {5, 11, 4},
             {11, 10, 2}, {10, 7, 6}, {7, 1, 8},  {3, 9, 4},  {3, 4, 2},   {3, 2, 6}, {3, 6, 8},
             {3, 8, 9},   {4, 9, 5},  {2, 4, 11}, {6, 2, 10}, {8, 6, 7},   {9, 8, 1}};
  for(int level=0; level<levels; ++level) {
    std::map<uint64_t, uint32_t> middles;
    auto middle = [&](uint32_t a, uint32_t b) {
      const uint64_t key = (uint64_t(std::min(a, b))<<32) | std::max(a, b);
      const auto it = middles.find(key);
      if(it!=middles.end())
        return it->second;
      s.points.push_back(unit(s.points[a]+s.points[b]));
      const uint32_t id = uint32_t(s.points.size()-1);
      middles.emplace(key, id);
      return id;
      };
    std::vector<std::array<uint32_t, 3>> next;
    next.reserve(s.faces.size()*4);
    for(const auto& f:s.faces) {
      const uint32_t ab = middle(f[0], f[1]), bc = middle(f[1], f[2]), ca = middle(f[2], f[0]);
      next.push_back({f[0], ab, ca});
      next.push_back({f[1], bc, ab});
      next.push_back({f[2], ca, bc});
      next.push_back({ab, bc, ca});
      }
    s.faces = std::move(next);
    }
  // outward counter-clockwise, whatever the table above says
  for(auto& f:s.faces)
    if(dot(cross(s.points[f[1]]-s.points[f[0]], s.points[f[2]]-s.points[f[0]]), s.points[f[0]])<0)
      std::swap(f[1], f[2]);
  return s;
  }

struct Stone {
  const char* name;
  const char* file;
  uint32_t    seed;
  float       size[3];  // half sizes in metres before the relief
  float       lift;     // the centre's height over the ground, as a share of the half height
  float       relief;   // how far the surface wanders, as a share of the radius
  int         cuts;     // flat faces split off
  };

// A lump, a slab that lies flat, a dome with its lower half in the ground, and a small upright stone.
const Stone Stones[] = {
  {"ISLAND_ROCK_1", "rock_1", 811, {0.85f, 0.70f, 0.75f}, 0.72f, 0.24f, 7},
  {"ISLAND_ROCK_2", "rock_2", 821, {1.15f, 0.32f, 0.85f}, 0.55f, 0.16f, 5},
  {"ISLAND_ROCK_3", "rock_3", 823, {1.25f, 0.95f, 1.10f}, 0.05f, 0.20f, 6},
  {"ISLAND_ROCK_4", "rock_4", 827, {0.30f, 0.42f, 0.27f}, 0.70f, 0.22f, 5},
  };

Shape stone(const Stone& spec) {
  Sphere s = icosphere(4);
  struct Cut {
    V3    n;
    float at = 0;
    };
  std::vector<Cut> cuts;
  for(int i=0; i<spec.cuts; ++i) {
    const uint32_t h = hash32(spec.seed*31U + uint32_t(i)*977U);
    const float    a = 6.28318530718f*hashUnit(h);
    const float    y = 1.6f*hashUnit(hash32(h))-0.6f; // more cuts above than below
    const float    r = std::sqrt(std::max(0.f, 1.f-std::min(1.f, y*y)));
    cuts.push_back({unit({r*std::cos(a), std::min(1.f, y), r*std::sin(a)}), 0.72f+0.2f*hashUnit(hash32(h^0x55U))});
    }
  const float ox = 17.f*hashUnit(hash32(spec.seed)), oy = 17.f*hashUnit(hash32(spec.seed+1U));
  const float oz = 17.f*hashUnit(hash32(spec.seed+2U));
  std::vector<V3> points(s.points.size());
  for(size_t i=0; i<s.points.size(); ++i) {
    const V3& d = s.points[i];
    V3 p = d*(1.f + spec.relief*fbm3(d.x*1.4f+ox, d.y*1.4f+oy, d.z*1.4f+oz, spec.seed, 3));
    // a split face: what stands over the plane is pressed onto it
    for(const auto& cut:cuts) {
      const float over = dot(p, cut.n)-cut.at;
      if(over>0)
        p = p - cut.n*(over*0.9f);
      }
    // weathering of a few centimetres, kept off the scale of the big shape
    p = p*(1.f + 0.035f*fbm3(d.x*6.f+oz, d.y*6.f+ox, d.z*6.f+oy, spec.seed+7U, 3));
    points[i] = {p.x*spec.size[0], p.y*spec.size[1] + spec.lift*spec.size[1], p.z*spec.size[2]};
    }
  std::vector<V3> normals(points.size());
  for(const auto& f:s.faces) {
    const V3 n = cross(points[f[1]]-points[f[0]], points[f[2]]-points[f[0]]); // weighted by the face's area
    for(const uint32_t v:f)
      normals[v] = normals[v]+n;
    }
  Shape shape;
  for(size_t i=0; i<points.size(); ++i)
    // the material lays its maps by world position; these are for a reader that does not
    shape.add(points[i], unit(normals[i]), points[i].x/2.5f, points[i].z/2.5f);
  for(const auto& f:s.faces)
    shape.index.insert(shape.index.end(), {f[0], f[1], f[2]});
  return shape;
  }

// --- the shed -------------------------------------------------------------------------------------------------------

// A board: `along` is its length, `side` and `up` its section. The texture is four boards across u with the grain
// along v, a metre a repeat: every face takes one board of it, so no face carries a seam.
void board(Shape& shape, const V3& centre, const V3& along, const V3& up, float length, float width, float thick,
           uint32_t seed) {
  const V3 a = unit(along);
  const V3 s = unit(cross(up, a));
  const V3 u = cross(a, s);
  const V3    axis[3] = {a, u, s};
  const float half[3] = {length*0.5f, thick*0.5f, width*0.5f};
  for(int face=0; face<6; ++face) {
    const int   n    = face/2;
    const float sign = (face%2)==0 ? 1.f : -1.f;
    // the grain runs along the board; on its ends, along the wider side of the section
    const int   t1   = n==0 ? (width>=thick ? 2 : 1) : 0;
    const int   t2   = 3-n-t1;
    const V3    normal = axis[n]*sign;
    const uint32_t h   = hash32(seed*131U + uint32_t(face)*7919U);
    const float u0 = 0.25f*float(h & 3U) + 0.02f, u1 = u0 + 0.21f;
    const float v0 = hashUnit(hash32(h));
    const float corner[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    uint32_t id[4];
    for(int c=0; c<4; ++c) {
      const V3 p = centre + normal*half[n] + axis[t1]*(corner[c][0]*half[t1]) + axis[t2]*(corner[c][1]*half[t2]);
      id[c] = shape.add(p, normal, corner[c][1]<0 ? u0 : u1, v0 + (corner[c][0]+1.f)*half[t1]);
      }
    const bool ccw = dot(cross(axis[t1], axis[t2]), normal)>0;
    if(ccw)
      shape.index.insert(shape.index.end(), {id[0], id[1], id[2], id[0], id[2], id[3]}); else
      shape.index.insert(shape.index.end(), {id[0], id[2], id[1], id[0], id[3], id[2]});
    }
  }

// Four posts, a beam over each pair, three rafters down the slope and boards across them. 3.0 x 2.2 m between the
// posts, 2.3 m high in front and 1.9 m behind; the posts go 0.4 m into the ground, so it stands on a slope too.
Shape shed() {
  constexpr float HalfX = 1.5f, HalfZ = 1.1f, Front = 2.3f, Back = 1.9f, Post = 0.14f, Sunk = 0.4f;
  constexpr float BeamHigh = 0.16f, BeamWide = 0.12f, Rafter = 0.10f, Plank = 0.2f, PlankThick = 0.03f;
  Shape shape;
  uint32_t seed = 907;
  for(const float x:{-HalfX, HalfX}) {
    board(shape, {x, (Front-Sunk)*0.5f, HalfZ},  {0, 1, 0}, {0, 0, 1}, Front+Sunk, Post, Post, seed++);
    board(shape, {x, (Back-Sunk)*0.5f,  -HalfZ}, {0, 1, 0}, {0, 0, 1}, Back+Sunk,  Post, Post, seed++);
    }
  board(shape, {0, Front+BeamHigh*0.5f, HalfZ},  {1, 0, 0}, {0, 1, 0}, 2.f*HalfX+0.5f, BeamWide, BeamHigh, seed++);
  board(shape, {0, Back+BeamHigh*0.5f,  -HalfZ}, {1, 0, 0}, {0, 1, 0}, 2.f*HalfX+0.5f, BeamWide, BeamHigh, seed++);

  // the roof's slope: from the back beam's top up to the front beam's
  const V3    rise  = unit({0, Front-Back, 2.f*HalfZ});
  const V3    out   = unit(cross(rise, {1, 0, 0})); // the roof's upper side
  const float run   = std::sqrt((Front-Back)*(Front-Back) + 4.f*HalfZ*HalfZ) + 0.7f; // with the eaves
  const V3    ridge = {0, (Front+Back)*0.5f + BeamHigh, 0};
  for(const float x:{-HalfX+0.05f, 0.f, HalfX-0.05f})
    board(shape, ridge + V3{x, 0, 0} + out*(Rafter*0.5f), rise, out, run, 0.08f, Rafter, seed++);
  const int count = int(run/(Plank+0.006f));
  for(int i=0; i<count; ++i) {
    const uint32_t h = hash32(seed++);
    // hand-laid: every board a little off in length and in line
    const float shift = 0.05f*(hashUnit(h)-0.5f);
    const float skew  = 0.012f*(hashUnit(hash32(h))-0.5f);
    const float where = (float(i)+0.5f)*(Plank+0.006f) - 0.5f*float(count)*(Plank+0.006f);
    board(shape, ridge + V3{shift, 0, 0} + rise*where + out*(Rafter + PlankThick*0.5f), unit(V3{1, 0, 0}+rise*skew),
          out, 2.f*HalfX + 0.7f + 0.08f*(hashUnit(hash32(h^0x77U))-0.5f), Plank, PlankThick, h);
    }
  return shape;
  }

PropNote writeGlb(const std::filesystem::path& path, const char* name, const Shape& shape, const char* material,
                  const char* group) {
  PropNote note;
  note.name      = name;
  note.triangles = shape.index.size()/3;
  for(int a=0; a<3; ++a)
    note.min[a] = note.max[a] = shape.position[size_t(a)];
  for(size_t i=0; i<shape.position.size(); ++i) {
    note.min[i%3] = std::min(note.min[i%3], shape.position[i]);
    note.max[i%3] = std::max(note.max[i%3], shape.position[i]);
    }
  std::string minMax = ",\"min\":[";
  for(int a=0; a<3; ++a)
    minMax += (a>0 ? "," : "") + gothicglb::jsonFloat(note.min[a]);
  minMax += "],\"max\":[";
  for(int a=0; a<3; ++a)
    minMax += (a>0 ? "," : "") + gothicglb::jsonFloat(note.max[a]);
  minMax += "]";

  gothicglb::GlbBuilder glb;
  const size_t position = glb.accessor(shape.position, 5126, "VEC3", 3, false, 34962, minMax);
  const size_t normal   = glb.accessor(shape.normal,   5126, "VEC3", 3, false, 34962);
  const size_t uv       = glb.accessor(shape.uv,       5126, "VEC2", 2, false, 34962);
  const size_t index    = glb.accessor(shape.index,    5125, "SCALAR", 1, false, 34963);
  const std::string json =
    std::string("{\"asset\":{\"version\":\"2.0\",\"generator\":\"island-content\"},\"scene\":0,") +
    "\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"mesh\":0,\"name\":" + gothicglb::jsonString(name) + "}]," +
    "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":" + std::to_string(position) +
    ",\"NORMAL\":" + std::to_string(normal) + ",\"TEXCOORD_0\":" + std::to_string(uv) +
    "},\"indices\":" + std::to_string(index) + ",\"material\":0}]}]," +
    "\"materials\":[{\"name\":" + gothicglb::jsonString(material) +
    ",\"alphaMode\":\"OPAQUE\",\"extras\":{\"group\":" + gothicglb::jsonString(group) + "}}]," +
    "\"bufferViews\":" + glb.viewsJson() + ",\"accessors\":" + glb.accessorsJson() +
    ",\"buffers\":[{\"byteLength\":" + std::to_string(glb.bin.size()) + "}]}";
  const auto bytes = glb.finish(json);
  std::filesystem::create_directories(path.parent_path());
  gothicglb::writeFile(path, bytes.data(), bytes.size());
  return note;
  }

}

std::vector<PropNote> writeProps(const std::filesystem::path& dir) {
  std::vector<PropNote> notes;
  std::string json = "{\n";
  for(const auto& spec:Stones) {
    notes.push_back(writeGlb(dir/"island"/(std::string(spec.file)+".glb"), spec.name, stone(spec), BoulderMaterial,
                             "STONE"));
    json += std::string("  \"")+spec.name+"\": {\"glb\":\"island/"+spec.file+".glb\"},\n";
    }
  notes.push_back(writeGlb(dir/"island/shed.glb", "ISLAND_SHED", shed(), WoodMaterial, "WOOD"));
  json += "  \"ISLAND_SHED\": {\"glb\":\"island/shed.glb\"}\n}\n";

  std::ofstream out(dir/"meshes.json", std::ios::binary | std::ios::trunc);
  out << json;
  if(!out.flush())
    throw std::runtime_error("island: unable to write "+(dir/"meshes.json").string());
  return notes;
  }

}

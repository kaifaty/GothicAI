// The content of the empty island (engine layers, step 8.5: reports/engine/2026-11-13-empty-island/island-content.md).
// What island-data wrote into <build>/island/ is read the engine's own way, with no Gothic, no base and no sound:
// the world through readWorldChunks, the materials, the props and the sky through Resources by their names. The
// ground is then measured as it was read (the beach, the hill, the stream, the way up), and drawn by
// Renderer::draw at noon from the views of the note.
//
//   gothic-island-content-test          # ISLAND_SHOTS_DIR=<folder> also saves the frames there as PNG
//
// Every include here is the platform's or the island's (scripts/engine_layers.py layer_of), or a third-party
// library's.

#include "platform_services.h"

#include "camera.h"
#include "resources.h"
#include "worldchunksruntime.h"
#include "graphics/renderer.h"
#include "graphics/worldview.h"
#include "graphics/mesh/submesh/packedmesh.h"

#include "islandcontent.h"
#include "islandterrain.h"

#include <Tempest/CommandBuffer>
#include <Tempest/Device>
#include <Tempest/Fence>
#include <Tempest/Pixmap>
#if defined(__APPLE__)
#include <Tempest/MetalApi>
#else
#include <Tempest/VulkanApi>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <unistd.h>

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

float degrees(float radians) {
  return radians*180.f/Pi;
  }

const std::filesystem::path& islandDir() {
  static const std::filesystem::path dir = ISLAND_DATA_DIR;
  return dir;
  }

// Empty folders in place of the installation and every base location; the content and the textures are the
// island's. Nothing else of the machine is read: GOTHIC_DIR is unset, the base, its cache and store are empty.
struct Fixture {
  std::filesystem::path root;

  Fixture() {
    std::string pattern = (std::filesystem::temp_directory_path()/"gothic-island-content-XXXXXX").string();
    if(mkdtemp(pattern.data())==nullptr)
      throw std::runtime_error("unable to create a temporary folder");
    root = pattern;
    for(const auto* dir:{"game", "base", "base-cache", "base-store"})
      std::filesystem::create_directories(root/dir);
    unsetenv("GOTHIC_DIR");
    unsetenv("OPENGOTHIC_BASE_FINGERPRINT");
    unsetenv("OPENGOTHIC_VIDEOS_FINGERPRINT");
    unsetenv("OPENGOTHIC_CENSUS");
    setenv("OPENGOTHIC_BASE",       (root/"base").c_str(),            1);
    setenv("OPENGOTHIC_BASE_CACHE", (root/"base-cache").c_str(),      1);
    setenv("OPENGOTHIC_BASE_STORE", (root/"base-store").c_str(),      1);
    setenv("OPENGOTHIC_CONTENT",    (islandDir()/"content").c_str(),  1);
    setenv("OPENGOTHIC_TEXTURES",   (islandDir()/"textures").c_str(), 1);
    }

  ~Fixture() {
    std::error_code error;
    std::filesystem::remove_all(root, error);
    }
  };

// --- the chunks ---------------------------------------------------------------------------------------------------

// Every chunk is read; triangles, materials and the chunks' borders are those the generator's mesh has.
void checkChunks(const asset::WorldMesh& world, const ChunkTable& table) {
  const auto written = canonicalWorldMesh(island::terrain().mesh());
  const size_t triangles = written.polygons.material_indices.size();
  require(triangles>=200000 && triangles<=400000, text("the ground must have 200-400 thousand triangles: %zu", triangles));
  const auto difference = compareWorldMeshes(written, world);
  require(difference.empty(), "the chunks must give the mesh that was written: "+difference);

  require(world.materials.size()==island::GroundCount, "the world must have the ground's materials");
  for(uint32_t g=0; g<island::GroundCount; ++g) {
    const auto& mat   = world.materials[g];
    const bool  water = g==island::SeaWater || g==island::StreamWater;
    require(mat.name==island::groundName(island::Ground(g)) && mat.texture==mat.name+".TGA",
            "material "+std::to_string(g)+" must keep its name and texture: "+mat.name+", "+mat.texture);
    require((mat.group==asset::MaterialGroup::WATER)==water && (mat.alpha_func==asset::AlphaFunction::BLEND)==water,
            "only the water is of the water group and blended: "+mat.name);
    }

  // by the centroid's 64 m cell, as the writer cuts
  struct Cell {
    uint32_t    triangles = 0;
    asset::Vec3 min {}, max {};
    };
  std::map<std::pair<int32_t,int32_t>,Cell> cells;
  for(size_t t=0; t<triangles; ++t) {
    const asset::Vec3* v[3] = {};
    double cx = 0, cz = 0;
    for(size_t c=0; c<3; ++c) {
      v[c] = &written.vertices[written.polygons.vertex_indices[t*3+c]];
      cx += double(v[c]->x);
      cz += double(v[c]->z);
      }
    const auto id   = ChunkId::fromPosition(float(cx/3.0), float(cz/3.0));
    auto&      cell = cells[{id.x, id.z}];
    if(cell.triangles==0)
      cell.min = cell.max = *v[0];
    for(const auto* p:v)
      for(unsigned axis=0; axis<3; ++axis) {
        cell.min[axis] = std::min(cell.min[axis], (*p)[axis]);
        cell.max[axis] = std::max(cell.max[axis], (*p)[axis]);
        }
    ++cell.triangles;
    }
  const size_t side = size_t(island::ChunkSide);
  require(cells.size()==side*side && table.entries.size()==side*side,
          text("the field is %zu x %zu chunks: %zu written, %zu read", side, side, cells.size(), table.entries.size()));
  require(table.triangleChunks.size()==triangles, "every triangle belongs to a chunk");
  uint64_t sum = 0;
  for(const auto& entry:table.entries) {
    const auto name = text("chunk %d_%d", entry.id.x, entry.id.z);
    require(std::abs(entry.id.x*2+1)<int32_t(side) && std::abs(entry.id.z*2+1)<int32_t(side), name+" lies off the field");
    require(std::filesystem::is_regular_file(islandDir()/"world"/entry.file), name+" has no file "+entry.file);
    const auto it = cells.find({entry.id.x, entry.id.z});
    require(it!=cells.end(), name+" was not written");
    const auto& cell = it->second;
    require(entry.triangleCount==cell.triangles,
            text("%s must have %u triangles: %u", name.c_str(), cell.triangles, entry.triangleCount));
    for(unsigned axis=0; axis<3; ++axis)
      require(entry.min[axis]==cell.min[axis] && entry.max[axis]==cell.max[axis],
              text("%s: borders on axis %u must be %.2f..%.2f: %.2f..%.2f", name.c_str(), axis, double(cell.min[axis]),
                   double(cell.max[axis]), double(entry.min[axis]), double(entry.max[axis])));
    sum += entry.triangleCount;
    }
  require(sum==triangles, "the chunks together hold every triangle");
  std::cout << text("chunks: %zu, triangles %zu, vertices %zu\n", table.entries.size(), triangles, world.vertices.size());
  }

// --- the ground as it was read -------------------------------------------------------------------------------------

// The ground's triangles (no water) in metres, found by the metre cell their centroid lies in: every triangle is a
// piece of one cell of the generator's grid.
struct Land {
  struct Tri {
    float    x[3] = {}, y[3] = {}, z[3] = {};
    uint32_t v[3] = {};
    uint32_t material = 0;

    float slopeDegrees() const {
      const float ax = x[1]-x[0], ay = y[1]-y[0], az = z[1]-z[0];
      const float bx = x[2]-x[0], by = y[2]-y[0], bz = z[2]-z[0];
      const float nx = ay*bz-az*by, ny = az*bx-ax*bz, nz = ax*by-ay*bx;
      const float l  = std::sqrt(nx*nx+ny*ny+nz*nz);
      return l>0 ? degrees(std::acos(std::min(1.f, std::abs(ny)/l))) : 90.f;
      }
    };

  std::vector<Tri>      tris;
  std::vector<uint32_t> start, order; // triangles of a cell: order[start[cell]..start[cell+1])
  int                   side = int(2.f*island::FieldHalf);

  explicit Land(const asset::WorldMesh& world) {
    const size_t count = world.polygons.material_indices.size();
    for(size_t t=0; t<count; ++t) {
      const uint32_t material = world.polygons.material_indices[t];
      if(material==island::SeaWater || material==island::StreamWater)
        continue;
      Tri tri;
      tri.material = material;
      for(size_t c=0; c<3; ++c) {
        tri.v[c] = world.polygons.vertex_indices[t*3+c];
        const auto& p = world.vertices[tri.v[c]];
        tri.x[c] = p.x*0.01f;
        tri.y[c] = p.y*0.01f;
        tri.z[c] = p.z*0.01f;
        }
      tris.push_back(tri);
      }
    start.assign(size_t(side)*size_t(side)+1, 0);
    for(const auto& tri:tris)
      ++start[cellOf(tri)+1];
    for(size_t i=1; i<start.size(); ++i)
      start[i] += start[i-1];
    order.resize(tris.size());
    auto next = start;
    for(size_t t=0; t<tris.size(); ++t)
      order[next[cellOf(tris[t])]++] = uint32_t(t);
    }

  int clampCell(float metres) const {
    return std::clamp(int(std::floor(metres+island::FieldHalf)), 0, side-1);
    }

  size_t cellOf(const Tri& tri) const {
    const float cx = (tri.x[0]+tri.x[1]+tri.x[2])/3.f, cz = (tri.z[0]+tri.z[1]+tri.z[2])/3.f;
    return size_t(clampCell(cz))*size_t(side)+size_t(clampCell(cx));
    }

  // The triangle over (x, z) and the ground's height there; nullptr off the field.
  const Tri* at(float x, float z, float& height) const {
    const int ci = clampCell(x), cj = clampCell(z);
    for(int ring=0; ring<2; ++ring)
      for(int j=cj-ring; j<=cj+ring; ++j)
        for(int i=ci-ring; i<=ci+ring; ++i) {
          if(i<0 || j<0 || i>=side || j>=side || (ring==1 && i==ci && j==cj))
            continue;
          const size_t cell = size_t(j)*size_t(side)+size_t(i);
          for(uint32_t k=start[cell]; k<start[cell+1]; ++k) {
            const auto& t = tris[order[k]];
            const float d = (t.z[1]-t.z[2])*(t.x[0]-t.x[2]) + (t.x[2]-t.x[1])*(t.z[0]-t.z[2]);
            if(d==0)
              continue;
            const float a = ((t.z[1]-t.z[2])*(x-t.x[2]) + (t.x[2]-t.x[1])*(z-t.z[2]))/d;
            const float b = ((t.z[2]-t.z[0])*(x-t.x[2]) + (t.x[0]-t.x[2])*(z-t.z[2]))/d;
            const float c = 1.f-a-b;
            if(a<-1e-3f || b<-1e-3f || c<-1e-3f)
              continue;
            height = a*t.y[0]+b*t.y[1]+c*t.y[2];
            return &t;
            }
          }
    return nullptr;
    }

  float height(float x, float z) const {
    float h = 0;
    require(at(x, z, h)!=nullptr, text("the ground must cover (%.2f, %.2f)", double(x), double(z)));
    return h;
    }

  // Degrees of the steepest fall at (x, z), over a metre each way.
  float slopeDegrees(float x, float z) const {
    const float gx = (height(x+1.f, z)-height(x-1.f, z))*0.5f;
    const float gz = (height(x, z+1.f)-height(x, z-1.f))*0.5f;
    return degrees(std::atan(std::sqrt(gx*gx+gz*gz)));
    }
  };

// The field, the sea around the land and the summit, by the note's table.
void checkField(const asset::WorldMesh& world, const Land& land, float& summitX, float& summitZ) {
  const float half = island::FieldHalf;
  float minX = 1e9f, maxX = -1e9f, minZ = 1e9f, maxZ = -1e9f;
  for(const auto& v:world.vertices) {
    minX = std::min(minX, v.x*0.01f);
    maxX = std::max(maxX, v.x*0.01f);
    minZ = std::min(minZ, v.z*0.01f);
    maxZ = std::max(maxZ, v.z*0.01f);
    }
  require(minX==-half && maxX==half && minZ==-half && maxZ==half,
          text("the field is %.0f m each way from the origin: x %.2f..%.2f, z %.2f..%.2f", double(half), double(minX),
               double(maxX), double(minZ), double(maxZ)));

  float summit = -1e9f, reach = 0, across[2] = {1e9f, -1e9f}, edgeTop = -1e9f, edgeBottom = 1e9f;
  for(const auto& tri:land.tris)
    for(size_t c=0; c<3; ++c) {
      const float x = tri.x[c], y = tri.y[c], z = tri.z[c];
      if(y>summit) {
        summit  = y;
        summitX = x;
        summitZ = z;
        }
      if(y>0) {
        reach     = std::max(reach, std::max(std::abs(x), std::abs(z)));
        across[0] = std::min(across[0], x);
        across[1] = std::max(across[1], x);
        }
      if(std::abs(x)==half || std::abs(z)==half) {
        edgeTop    = std::max(edgeTop, y);
        edgeBottom = std::min(edgeBottom, y);
        }
      }
  std::cout << text("field: land %.0f m across along x, %.1f m of sea at the nearest edge, bottom at the edges %.1f..%.1f m, "
                    "summit %.1f m at (%.0f, %.0f)\n", double(across[1]-across[0]), double(half-reach),
                    double(edgeBottom), double(edgeTop), double(summit), double(summitX), double(summitZ));
  require(half-reach>=40.f, text("the sea must be at least 40 m wide at the field's edges: %.1f", double(half-reach)));
  require(edgeTop<=-5.f && edgeBottom>=-10.5f,
          text("the sea bottom at the edges must lie 5-10 m deep: %.1f..%.1f", double(edgeBottom), double(edgeTop)));
  require(summit>=25.f && summit<=35.f, text("the summit must be 25-35 m over the sea: %.1f", double(summit)));
  }

// The beach at the water: 1-4 degrees all around the island, the stream's mouth aside.
void checkBeach(const Land& land) {
  const auto& stream = island::terrain().stream();
  float low = 1e9f, high = -1e9f;
  int   rays = 0;
  for(int step=0; step<120; ++step) {
    const float a  = float(step)*2.f*Pi/120.f;
    const float dx = std::cos(a), dz = std::sin(a);
    // the waterline: the first step into the sea on the way out
    float r = 60.f;
    while(r<island::FieldHalf-2.f && land.height(dx*r, dz*r)>0)
      r += 0.25f;
    require(r<island::FieldHalf-2.f, text("the shore must lie inside the field at %.0f degrees", double(degrees(a))));
    bool mouth = false;
    for(const auto& p:stream)
      mouth = mouth || std::hypot(p.x-dx*r, p.z-dz*r)<15.f;
    if(mouth)
      continue;
    ++rays;
    for(float inland:{1.5f, 3.f, 5.f}) {
      const float slope = land.slopeDegrees(dx*(r-inland), dz*(r-inland));
      low  = std::min(low, slope);
      high = std::max(high, slope);
      require(slope>=1.f && slope<=4.f, text("the beach must rise 1-4 degrees at the water: %.2f degrees %.1f m inland at "
                                             "%.0f degrees of the shore", double(slope), double(inland), double(degrees(a))));
      }
    }
  require(rays>=100, "the beach must be measured all around the island");
  std::cout << text("beach: %.2f..%.2f degrees at the water on %d rays\n", double(low), double(high), rays);
  }

// The stream: its bed in the ground that was read never rises on the way down.
void checkStream(const Land& land) {
  const auto& stream = island::terrain().stream();
  require(stream.size()>=100, "the stream must be at least 100 m long");
  // rise: the most that the bed climbs back between any two points, the lower one upstream
  float lowest = 0, rise = 0, steepest = 0, spring = 0;
  size_t onLand = 0;
  std::vector<float> bed(stream.size());
  for(size_t i=0; i<stream.size(); ++i) {
    float h = 0;
    const auto* tri = land.at(stream[i].x, stream[i].z, h);
    require(tri!=nullptr, "the stream must lie on the ground");
    bed[i] = h;
    if(i==0)
      spring = lowest = h;
    rise   = std::max(rise, h-lowest);
    lowest = std::min(lowest, h);
    if(h>0) {
      onLand = i+1;
      require(tri->material==island::StreamBed, text("the stream's bed must be of its own material %zu m down", i));
      }
    }
  for(size_t i=0; i+20<onLand; ++i)
    steepest = std::max(steepest, (bed[i]-bed[i+20])/20.f);
  std::cout << text("stream: %zu m, %zu m on land, spring at %.2f m, steepest 20 m fall %.1f %%, the bed climbs back "
                    "%.1f mm at most\n", stream.size()-1, onLand, double(spring), double(steepest)*100.0,
                    double(rise)*1000.0);
  // Over the whole length, not a step: a slow even climb adds up. The centre line is sampled on triangles of a
  // metre, so 3 mm is the mesh's own chord, not a rise.
  require(rise<=0.003f, text("the stream's bed must not rise downstream: %.1f mm", double(rise)*1000.0));
  require(spring>=3.f, text("the spring must lie on the hill's slope: %.2f m", double(spring)));
  require(steepest>=0.03f && steepest<=0.08f,
          text("the stream's steep stretch must fall 3-8 %%: %.1f %%", double(steepest)*100.0));
  }

// The way up: from the summit, over ground no steeper than the limit and not under the sea, the beach is reached
// all around the island. The platform has no slope limit of its own: it is an argument of VirtualCharacter::move,
// and the game passes the guild's slide angle. 35 degrees is under any of them.
constexpr float WalkSlope = 35.f;

void checkWayUp(const Land& land, float summitX, float summitZ) {
  std::unordered_map<uint64_t,std::array<uint32_t,2>> edges;
  edges.reserve(land.tris.size()*2);
  constexpr uint32_t None = 0xFFFFFFFF;
  for(size_t t=0; t<land.tris.size(); ++t)
    for(size_t c=0; c<3; ++c) {
      const uint32_t a = land.tris[t].v[c], b = land.tris[t].v[(c+1)%3];
      auto& pair = edges.try_emplace(uint64_t(std::min(a, b))<<32 | std::max(a, b), std::array<uint32_t,2>{None, None}).first->second;
      pair[pair[0]==None ? 0 : 1] = uint32_t(t);
      }
  std::vector<float> slope(land.tris.size());
  std::vector<bool>  dry(land.tris.size());
  for(size_t t=0; t<land.tris.size(); ++t) {
    slope[t] = land.tris[t].slopeDegrees();
    dry[t]   = std::min({land.tris[t].y[0], land.tris[t].y[1], land.tris[t].y[2]})>-0.5f;
    }
  float h = 0;
  const auto* top = land.at(summitX, summitZ, h);
  require(top!=nullptr, "the summit must lie on the ground");
  const auto from = uint32_t(top-land.tris.data());

  // the share of the dry beach that the summit reaches under `limit`
  auto reached = [&](float limit) {
    std::vector<bool>     seen(land.tris.size());
    std::vector<uint32_t> queue = {from};
    seen[from] = true;
    for(size_t at=0; at<queue.size(); ++at) {
      const auto& tri = land.tris[queue[at]];
      for(size_t c=0; c<3; ++c) {
        const uint32_t a = tri.v[c], b = tri.v[(c+1)%3];
        for(uint32_t next:edges.at(uint64_t(std::min(a, b))<<32 | std::max(a, b)))
          if(next!=None && !seen[next] && dry[next] && slope[next]<limit) {
            seen[next] = true;
            queue.push_back(next);
            }
        }
      }
    size_t beach = 0, got = 0;
    for(size_t t=0; t<land.tris.size(); ++t)
      if(land.tris[t].material==island::SandDry) {
        ++beach;
        got += seen[t] ? 1 : 0;
        }
    return beach>0 ? float(got)/float(beach) : 0.f;
    };
  // the gentlest limit that still lets the summit reach the beach
  float low = 0, high = 90;
  for(int pass=0; pass<10; ++pass) {
    const float middle = 0.5f*(low+high);
    (reached(middle)>0 ? high : low) = middle;
    }
  const float share = reached(WalkSlope);
  std::cout << text("way up: at %.0f degrees the summit reaches %.1f %% of the dry beach; a way exists from %.1f degrees\n",
                    double(WalkSlope), double(share)*100.0, double(high));
  require(share>=0.99f, text("a way no steeper than %.0f degrees must lead from the beach to the summit: %.1f %% of the "
                             "beach reaches it, a way needs %.1f degrees", double(WalkSlope), double(share)*100.0, double(high)));
  }

// --- names ---------------------------------------------------------------------------------------------------------

// Materials and the sky are found by the names the engine asks for.
void checkNames() {
  for(uint32_t g=0; g<island::GroundCount; ++g) {
    const std::string name = std::string(island::groundName(island::Ground(g)))+".TGA";
    const auto maps = Resources::loadMaterialMaps(name);
    require(maps.present && maps.albedo!=nullptr, "the material must have its albedo: "+name);
    if(g==island::SeaWater || g==island::StreamWater)
      continue;
    require(maps.normal!=nullptr && maps.orm!=nullptr, "the material must have its normal and ORM maps: "+name);
    require(maps.antitile!=Material::Antitile::Off, "the ground's material must break its tiling: "+name);
    for(const auto* map:{maps.albedo, maps.normal, maps.orm})
      require(map->w()>=512 && map->w()==map->h() && map->mipCount()>=8,
              text("the maps of %s must be square, 512 and more, with their mips: %d x %d, %u mips", name.c_str(), map->w(),
                   map->h(), unsigned(map->mipCount())));
    }
  for(const char* prop:{island::BoulderMaterial, island::WoodMaterial}) {
    const std::string name = std::string(prop)+".TGA";
    const auto maps = Resources::loadMaterialMaps(name);
    require(maps.present && maps.albedo!=nullptr && maps.normal!=nullptr && maps.orm!=nullptr,
            "the prop's material must have its maps: "+name);
    }
  require(Resources::loadMaterialMaps(std::string(island::BoulderMaterial)+".TGA").projectionTile>0,
          "loose stones take the rock by world position");
  for(const char* name:island::SkyTextures)
    require(Resources::loadTexture(name)!=nullptr, std::string("the sky's texture must be found: ")+name);
  }

// --- frames --------------------------------------------------------------------------------------------------------

struct Rgb {
  float r = 0, g = 0, b = 0;
  };

struct Window {
  int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  std::string name;
  };

struct Stats {
  Rgb   mean;
  float sand = 0, grass = 0, blue = 0; // shares of the pixels by their hue
  };

Stats measure(const Tempest::Pixmap& frame, const Window& w) {
  const auto* p = reinterpret_cast<const uint8_t*>(frame.data());
  Stats  s;
  size_t n = 0, sand = 0, grass = 0, blue = 0;
  for(int y=std::max(w.y0, 0); y<std::min(w.y1, int(frame.h())); ++y)
    for(int x=std::max(w.x0, 0); x<std::min(w.x1, int(frame.w())); ++x) {
      const auto* px = p + (size_t(y)*frame.w()+size_t(x))*4;
      const float r = px[0], g = px[1], b = px[2];
      s.mean.r += r;
      s.mean.g += g;
      s.mean.b += b;
      ++n;
      if(r>=g && g>=b && r-b>=20.f)
        ++sand;
      if(g>r && g-b>=20.f)
        ++grass;
      if(b>r && b-r>=20.f)
        ++blue;
      }
  require(n>=64, "the window must hold at least 64 pixels: "+w.name);
  s.mean.r /= float(n);
  s.mean.g /= float(n);
  s.mean.b /= float(n);
  s.sand  = float(sand)/float(n);
  s.grass = float(grass)/float(n);
  s.blue  = float(blue)/float(n);
  return s;
  }

std::string describe(const Window& w, const Stats& s) {
  return text("%s [%d..%d]x[%d..%d]: mean %.0f %.0f %.0f, sand %.2f, grass %.2f, blue %.2f", w.name.c_str(), w.x0, w.x1,
              w.y0, w.y1, double(s.mean.r), double(s.mean.g), double(s.mean.b), double(s.sand), double(s.grass),
              double(s.blue));
  }

// What a frame must show where: a hue that leads in the window.
enum class Hue { Sand, Grass, Blue };

struct Probe {
  std::string   name;
  Tempest::Vec3 at;       // the world's point, centimetres
  int           half = 0; // the window around its place on the screen, pixels each way
  Hue           hue = Hue::Sand;
  float         share = 0.6f;
  };

struct Shot {
  std::string        file;
  uint32_t           w = 0, h = 0;
  Tempest::Vec3      from;
  bool               down = false; // straight down; otherwise towards `to`
  Tempest::Vec3      to;
  float              near = 10.f;  // the near plane, centimetres (the camera's own by default)
  std::vector<Probe> probes;
  };

// Turns the camera at `from` so that `to` lies in the middle of the screen.
void lookAt(Camera& camera, const Tempest::Vec3& from, const Tempest::Vec3& to) {
  auto place = [&](float pitch, float yaw) {
    Camera::SaveState state = camera.saveState();
    state.origin      = from;
    state.target      = from;
    state.interTarget = from; // the shadows' cascades are laid around it
    state.angles      = {pitch, yaw, 0};
    camera.setSaveState(state);
    };
  auto miss = [&](float pitch, float yaw) {
    place(pitch, yaw);
    Tempest::Vec3 p = to;
    camera.viewProj().project(p);
    return p.z>0 && p.z<1 ? p.x*p.x+p.y*p.y : 1e9f;
    };
  float best = 1e9f, pitch = 0, yaw = 0;
  for(float p=-88; p<=88; p+=2)
    for(float y=0; y<360; y+=2)
      if(const float m=miss(p, y); m<best) {
        best  = m;
        pitch = p;
        yaw   = y;
        }
  for(float step:{0.25f, 0.02f}) {
    const float p0 = pitch, y0 = yaw;
    for(float p=p0-step*10; p<=p0+step*10; p+=step)
      for(float y=y0-step*10; y<=y0+step*10; y+=step)
        if(const float m=miss(p, y); m<best) {
          best  = m;
          pitch = p;
          yaw   = y;
          }
    }
  require(best<1e-4f, "the camera must find its target");
  place(pitch, yaw);
  }

Tempest::Vec3 onGround(const Land& land, float x, float z, float above) {
  return {x*100.f, (land.height(x, z)+above)*100.f, z*100.f};
  }

void drawCase(Tempest::Device& device, const Fixture& fixture, const asset::WorldMesh& world, const Land& land,
              float summitX, float summitZ, const std::filesystem::path& shotsDir) {
  Resources resources(device, Resources::Audio::Off);
  platform::Services services(fixture.root/"game");
  require(Resources::selectedBaseFingerprint().empty(), "no base is selected");
  checkNames();

  const auto& terrain = island::terrain();
  const auto& stream  = terrain.stream();

  {
  PackedMesh ground(world, PackedMesh::PK_VisualLnd);
  WorldView  view(ground, island::WorldName);

  // The props stand where the oblique view has them in front: the stones by the stream's lower reach, the shed on
  // the meadow beside it. The island's own program (step 8.6) places them for itself.
  size_t mouth = 0;
  while(mouth+1<stream.size() && terrain.shore(stream[mouth].x, stream[mouth].z)>0)
    ++mouth;
  require(mouth>=60, "the stream must run at least 60 m over land");
  auto beside = [&](size_t upstream, float aside, float& x, float& z) {
    const auto& p = stream[mouth-upstream];
    const auto& q = stream[mouth-upstream-1];
    const float tx = q.x-p.x, tz = q.z-p.z; // upstream
    const float l  = std::sqrt(tx*tx+tz*tz);
    x = p.x + tz/l*aside;
    z = p.z - tx/l*aside;
    };
  struct Placed {
    const char* name;
    size_t      upstream;
    float       aside, turn;
    float       foot; // the radius it stands on, metres
    };
  const Placed placed[] = {
    {"ISLAND_ROCK_1", 22,  3.6f,  20.f, 0.6f},  {"ISLAND_ROCK_2", 19, -3.8f, 110.f, 0.8f},
    {"ISLAND_ROCK_3", 34, -6.5f, 250.f, 0.9f},  {"ISLAND_ROCK_4", 20,  5.0f,  70.f, 0.25f},
    {"ISLAND_ROCK_4", 17, -2.6f, 200.f, 0.25f}, {"ISLAND_ROCK_2", 40,  5.0f, 320.f, 0.8f},
    {"ISLAND_SHED",   38, 11.0f,  35.f, 1.8f},
    };
  std::vector<MeshObjects::Mesh> props;
  const auto* fallback = Resources::loadDefaultTexture();
  for(const auto& p:placed) {
    float x = 0, z = 0;
    beside(p.upstream, p.aside, x, z);
    auto mesh = view.addStaticView(std::string(p.name)+".3DS");
    require(mesh.nodesCount()>0, std::string("the prop must be found by its name: ")+p.name);
    for(size_t n=0; n<mesh.nodesCount(); ++n)
      require(mesh.node(n).material().tex!=nullptr && mesh.node(n).material().tex!=fallback,
              std::string("the prop must be drawn with its own texture: ")+p.name);
    // on a slope it stands on the lowest ground under it: no air under its downhill side
    float ground = land.height(x, z);
    for(int i=0; i<8; ++i) {
      const float a = float(i)*0.785398f;
      ground = std::min(ground, land.height(x+p.foot*std::cos(a), z+p.foot*std::sin(a)));
      }
    auto at = Tempest::Matrix4x4::mkIdentity();
    at.translate(x*100.f, ground*100.f, z*100.f);
    at.rotateOY(p.turn);
    mesh.setObjMatrix(at);
    props.push_back(std::move(mesh));
    }

  // The corner of four chunks nearest to the middle of the dry beach.
  float cornerX = 0, cornerZ = 0, cornerMiss = 1e9f;
  for(int j=-island::ChunkSide/2+1; j<island::ChunkSide/2; ++j)
    for(int i=-island::ChunkSide/2+1; i<island::ChunkSide/2; ++i) {
      const float x = float(i)*64.f, z = float(j)*64.f;
      const float miss = std::abs(terrain.shore(x, z)-16.f);
      bool nearStream = false;
      for(const auto& p:stream)
        nearStream = nearStream || std::hypot(p.x-x, p.z-z)<50.f;
      if(miss<cornerMiss && !nearStream) {
        cornerMiss = miss;
        cornerX    = x;
        cornerZ    = z;
        }
      }
  require(cornerMiss<8.f, "a corner of four chunks must lie on the beach");
  // inland and seaward of the corner, along the shore's own fall
  const float e  = 1.f;
  float       nx = terrain.shore(cornerX+e, cornerZ)-terrain.shore(cornerX-e, cornerZ);
  float       nz = terrain.shore(cornerX, cornerZ+e)-terrain.shore(cornerX, cornerZ-e);
  const float nl = std::sqrt(nx*nx+nz*nz);
  nx /= nl;
  nz /= nl;
  const float shoreAt = terrain.shore(cornerX, cornerZ);
  std::cout << text("junction: chunks meet at (%.0f, %.0f), %.1f m from the water\n", double(cornerX), double(cornerZ),
                    double(shoreAt));

  // The oblique view: from the beach by the stream's mouth, a little over a man's height, up the stream to the hill.
  float eyeX = 0, eyeZ = 0;
  beside(9, -7.f, eyeX, eyeZ);
  float midX = 0, midZ = 0;
  beside(std::min<size_t>(mouth-1, 70), 0.f, midX, midZ);
  float sandX = 0, sandZ = 0, meadowX = 0, meadowZ = 0;
  beside(50, -12.f, meadowX, meadowZ);
  beside(17, -7.f, sandX, sandZ);

  std::vector<Shot> shots;
  {
    Shot s;
    s.file = "island-top.png";
    s.w    = 1200;
    s.h    = 1200;
    s.from = {0, 29000, 0};
    s.down = true;
    // The fog counts its air from the near plane and stretches it 50 times (fog_view_lut.comp): straight down from
    // 290 m its ray ends 14 km under the sea, where the haze's density has grown without bound, and the frame is one
    // colour of fog. With the near plane 40 m over the sea the fog is that of 40 m of air. A way around, not a
    // cure: the debt is issue #1342.
    s.near = s.from.y-4000.f;
    s.probes.push_back({"the meadow", onGround(land, -40.f, -70.f, 0), 24, Hue::Grass, 0.7f});
    s.probes.push_back({"the sea", {-17500, 0, 17500}, 24, Hue::Blue, 0.7f});
    s.probes.push_back({"the beach", onGround(land, cornerX-nx*(shoreAt-15.f), cornerZ-nz*(shoreAt-15.f), 0), 4, Hue::Sand, 0.7f});
    shots.push_back(std::move(s));
  }
  {
    Shot s;
    s.file = "island-junction.png";
    s.w    = 1600;
    s.h    = 1000;
    s.from = onGround(land, cornerX, cornerZ, 36.f);
    s.down = true;
    s.probes.push_back({"the dry sand", onGround(land, cornerX-nx*(shoreAt-15.f), cornerZ-nz*(shoreAt-15.f), 0), 40, Hue::Sand, 0.8f});
    s.probes.push_back({"the grass", onGround(land, cornerX+nx*(34.f-shoreAt), cornerZ+nz*(34.f-shoreAt), 0), 40, Hue::Grass, 0.8f});
    shots.push_back(std::move(s));
  }
  {
    Shot s;
    s.file = "island-beach.png";
    s.w    = 1600;
    s.h    = 900;
    s.from = onGround(land, eyeX, eyeZ, 2.6f);
    s.to   = onGround(land, midX, midZ, 5.f);
    s.probes.push_back({"the sand in front", onGround(land, sandX, sandZ, 0), 30, Hue::Sand, 0.7f});
    s.probes.push_back({"the meadow by the stream", onGround(land, meadowX, meadowZ, 0), 12, Hue::Grass, 0.6f});
    s.probes.push_back({"the hill's slope", onGround(land, summitX-22.f, summitZ+16.f, 0), 12, Hue::Grass, 0.4f}); // behind 130 m of haze
    s.probes.push_back({"the sky over the hill", onGround(land, summitX, summitZ, 45.f), 30, Hue::Blue, 0.7f});
    shots.push_back(std::move(s));
  }

  Camera camera;
  services.game.view = &view;
  services.game.cam  = &camera;
  Renderer renderer;
  renderer.onWorldChanged();
  uint32_t frameNo = 0;
  for(const auto& shot:shots) {
    camera.setViewport(shot.w, shot.h);
    camera.setNearClip(shot.near);
    if(shot.down) {
      Camera::SaveState state = camera.saveState();
      state.origin      = shot.from;
      state.target      = {shot.from.x, 0, shot.from.z};
      state.interTarget = state.target;
      state.angles      = {90, 90, 0};
      camera.setSaveState(state);
      } else {
      lookAt(camera, shot.from, shot.to);
      }
    camera.markCut();

    auto frame = device.attachment(Tempest::TextureFormat::RGBA8, shot.w, shot.h);
    // the exposure, TAA and the shadows settle on their history; the textures come in at their full size
    for(uint32_t i=0; i<48; ++i, ++frameNo) {
      const auto fId = uint8_t(frameNo%Resources::MaxFramesInFlight);
      Resources::resetRecycled(fId);
      Resources::updateTextures(nullptr);
      services.game.ticks += 16;
      Tempest::CommandBuffer cmd;
      {
      auto enc = cmd.startEncoding(device);
      renderer.draw(frame, enc, fId, view, camera);
      }
      device.submit(cmd).wait();
      }
    auto pixels = device.readPixels(frame);
    if(pixels.format()!=Tempest::TextureFormat::RGBA8)
      pixels = Tempest::Pixmap(pixels, Tempest::TextureFormat::RGBA8);
    if(!shotsDir.empty()) {
      const auto file = shotsDir/shot.file;
      pixels.save(file.string().c_str());
      require(std::filesystem::is_regular_file(file), "unable to write "+file.string());
      }

    const auto vp = camera.viewProj();
    for(const auto& probe:shot.probes) {
      Tempest::Vec3 p = probe.at;
      vp.project(p);
      require(std::abs(p.x)<1.f && std::abs(p.y)<1.f && p.z>0.f && p.z<1.f,
              shot.file+": "+probe.name+" must lie in the camera's view");
      const int px = int((p.x*0.5f+0.5f)*float(shot.w)), py = int((p.y*0.5f+0.5f)*float(shot.h));
      const Window window = {px-probe.half, py-probe.half, px+probe.half, py+probe.half, shot.file+": "+probe.name};
      const auto   stats  = measure(pixels, window);
      std::cout << describe(window, stats) << '\n';
      const float share = probe.hue==Hue::Sand ? stats.sand : probe.hue==Hue::Grass ? stats.grass : stats.blue;
      require(share>=probe.share, "the frame must show the hue of "+describe(window, stats));
      }
    }
  device.waitIdle();
  }
  }
}

int main(int argc, char**) {
  try {
    if(argc!=1)
      throw std::runtime_error("usage: gothic-island-content-test (ISLAND_SHOTS_DIR=<folder> saves the frames)");
    std::filesystem::path shotsDir;
    if(const char* dir=std::getenv("ISLAND_SHOTS_DIR"); dir!=nullptr && dir[0]!='\0') {
      shotsDir = std::filesystem::absolute(dir);
      std::filesystem::create_directories(shotsDir);
      }
    Fixture fixture;

    ChunkTable table;
    const auto world = readWorldChunks(islandDir()/"world", {}, &table);
    checkChunks(world, table);
    const Land land(world);
    float summitX = 0, summitZ = 0;
    checkField(world, land, summitX, summitZ);
    checkBeach(land);
    checkStream(land);
    checkWayUp(land, summitX, summitZ);

    platform::Arguments arguments(fixture.root/"game");
#if defined(__APPLE__)
    Tempest::MetalApi  api;
#else
    Tempest::VulkanApi api;
#endif
    Tempest::Device device(api);
    drawCase(device, fixture, world, land, summitX, summitZ, shotsDir);
    device.waitIdle();
    std::cout << "{\"test\":\"island_content\",\"passed\":true,\"assertions\":" << assertions << "}\n";
    return 0;
    }
  catch(const std::exception& error) {
    std::cerr << "island content test failed: " << error.what() << '\n';
    return 1;
    }
  }

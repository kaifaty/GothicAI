#pragma once

// The synthetic cloth world (the objects of worldcloth_contract.cpp as chunks), its three override sets and the
// helpers of the cooked world contracts that load it: the cooked path switch, the log capture, byte edits of cooked
// files and the scratch copy of a base world. Shared by worldcloth_cooked_contract.cpp and
// worldcollision_chunks_contract.cpp.

#include "gothicglb.h"
#include "physics/worldclothbundle.h"
#include "worldchunk_fixtures.h"
#include "worldchunk_strip.h"
#include "worldchunkbinary.h"
#include "worldchunkindex.h"
#include "worldchunks.h"

#include <Tempest/Log>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace WorldChunkFixtures {

inline const WorldChunkSource clothSource = {"CLOTH_CONTRACT.ZEN",7,0x0C107FEEu};

inline void setCookedPath(bool on) {
#ifdef _WIN32
  _putenv_s("OPENGOTHIC_WCB",on ? "" : "0");
#else
  if(on)
    unsetenv("OPENGOTHIC_WCB");
  else
    setenv("OPENGOTHIC_WCB","0",1);
#endif
  }

// Everything the engine logs while it lives; Tempest splits long lines, so the pieces are joined.
struct LogCapture {
  std::mutex  sync;
  std::string text;
  LogCapture() {
    Tempest::Log::setOutputCallback([this](Tempest::Log::Mode,const char* piece) {
      std::lock_guard guard(sync);
      text+=piece;
      });
    }
  ~LogCapture() { Tempest::Log::setOutputCallback({}); }
  std::string take() {
    std::lock_guard guard(sync);
    return std::exchange(text,{});
    }
  };

inline size_t tagged(const std::string& log, const std::string& tag) {
  size_t count=0;
  for(auto at=log.find(tag);at!=std::string::npos;at=log.find(tag,at+1))
    ++count;
  return count;
  }

inline std::vector<uint8_t> fileBytes(const std::filesystem::path& path) {
  return gothicglb::readFile(path);
  }

inline void writeBytes(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
  gothicglb::writeFile(path,bytes.data(),bytes.size());
  }

inline uint32_t get32(const std::vector<uint8_t>& bytes, size_t at) {
  uint32_t value=0;
  for(unsigned i=0;i<4;++i)
    value|=uint32_t(bytes[at+i])<<(8*i);
  return value;
  }

inline void put32(std::vector<uint8_t>& bytes, size_t at, uint32_t value) {
  for(unsigned i=0;i<4;++i)
    bytes[at+i]=uint8_t(value>>(8*i));
  }

// A GLB with four more bytes of JSON whitespace: the same mesh from a file of another length.
inline std::vector<uint8_t> padJson(std::vector<uint8_t> glb) {
  const auto json=get32(glb,12);
  glb.insert(glb.begin()+20+json,4,uint8_t(' '));
  put32(glb,12,json+4);
  put32(glb,8,uint32_t(glb.size()));
  return glb;
  }

// ---- the synthetic cloth world: the objects of worldcloth_contract.cpp ----

inline uint32_t addMaterial(asset::WorldMesh& mesh, const std::string& name, const std::string& texture) {
  asset::Material material;
  material.name = name;
  material.texture = texture;
  material.group = asset::MaterialGroup::WOOD;
  material.color = {12,34,56,178};
  material.smooth_angle = 47.25f;
  material.texture_scale = {128.f,256.f};
  material.texture_anim_fps = 7.5f;
  material.texture_anim_map_mode = asset::AnimationMapping::LINEAR;
  material.texture_anim_map_dir = {.25f,-.125f};
  material.disable_lightmap = true;
  material.dont_collapse = true;
  material.detail_object = "SYNTHETIC_DETAIL";
  material.detail_object_scale = .75f;
  material.force_occluder = true;
  material.environment_mapping = true;
  material.environment_mapping_strength = .125f;
  material.wave_mode = asset::WaveMode::WALL_AMBIENT;
  material.wave_speed = asset::WaveSpeed::SLOW;
  material.wave_max_amplitude = 11.5f;
  material.wave_grid_size = 91.f;
  material.ignore_sun = true;
  material.alpha_func = asset::AlphaFunction::BLEND;
  material.default_mapping = {.5f,.75f};
  const auto id = uint32_t(mesh.materials.size());
  mesh.materials.push_back(material);
  return id;
  }

inline void addTriangle(asset::WorldMesh& mesh, uint32_t material, const std::array<uint32_t,3>& indices) {
  const auto face = uint32_t(mesh.polygons.material_indices.size());
  // worldcloth_contract.cpp's flags (occluder, sector, relight, dynamically lit, LOD, normal axis), as .wcb packs them.
  const auto bits = (face%2)<<2 | ((face/2)%2)<<3 | ((face/3)%2)<<4 | ((face/5)%2)<<7 | ((face/7)%2)<<8 | (face%3)<<9;
  const auto flags = wcb::unpackFlags(uint16_t(bits),int16_t(100+face));
  mesh.polygons.material_indices.push_back(material);
  mesh.polygons.lightmap_indices.push_back(face%2 ? -1 : 0);
  mesh.polygons.flags.push_back(flags);
  for(size_t corner=0;corner<3;++corner) {
    const auto feature = uint32_t(mesh.features.size());
    mesh.features.push_back({{float(face)*.03125f+float(corner)*.125f,float(face)*.015625f-float(corner)*.25f},
                             0x10203040u+face*3+uint32_t(corner),{.25f,float(corner)*.125f,1.f}});
    mesh.polygons.vertex_indices.push_back(indices[corner]);
    mesh.polygons.feature_indices.push_back(feature);
    }
  }

// A double-sided quad x..x+200 wide, 300 high: four positions, four triangles.
inline uint32_t addBanner(asset::WorldMesh& mesh, uint32_t material, float x) {
  const auto base = uint32_t(mesh.vertices.size());
  mesh.vertices.insert(mesh.vertices.end(),{{x,600,0},{x+200,600,0},{x+200,300,0},{x,300,0}});
  for(const auto& tri:std::array<std::array<uint32_t,3>,4>{{{0,1,2},{0,2,3},{0,2,1},{0,3,2}}})
    addTriangle(mesh,material,{base+tri[0],base+tri[1],base+tri[2]});
  return base;
  }

inline void addFinial(asset::WorldMesh& mesh, uint32_t material) {
  const auto base = uint32_t(mesh.vertices.size());
  for(size_t ring=0;ring<2;++ring)
    for(size_t side=0;side<8;++side) {
      const float angle = float(side)*3.14159265358979323846f*.25f;
      mesh.vertices.push_back({100+18.f*std::cos(angle),650.f+float(ring)*90.f,18.f*std::sin(angle)});
      }
  for(uint32_t side=0;side<8;++side) {
    const auto next = (side+1)%8;
    addTriangle(mesh,material,{base+side,base+next,base+8+side});
    addTriangle(mesh,material,{base+next,base+8+next,base+8+side});
    }
  for(uint32_t corner=1;corner<7;++corner) {
    addTriangle(mesh,material,{base,base+corner+1,base+corner});
    addTriangle(mesh,material,{base+8,base+8+corner,base+8+corner+1});
    }
  }

inline uint32_t addGrid(asset::WorldMesh& mesh, uint32_t material, bool sail) {
  const uint32_t side = sail ? 5 : 3;
  const auto first = uint32_t(mesh.polygons.material_indices.size());
  const auto base = uint32_t(mesh.vertices.size());
  for(uint32_t row=0;row<side;++row)
    for(uint32_t col=0;col<side;++col) {
      if(sail)
        mesh.vertices.push_back({3000.f+float(col)*200.f,600.f+float(row)*30.f,
                                 20.f*std::sin(float(col)*3.14159265358979323846f*.25f)});
      else
        mesh.vertices.push_back({1200.f,200.f+float(row)*100.f,float(col)*150.f});
      }
  for(uint32_t row=0;row+1<side;++row)
    for(uint32_t col=0;col+1<side;++col) {
      const auto a = base+row*side+col, b = a+1, c = a+side, d = c+1;
      addTriangle(mesh,material,{a,b,c});
      addTriangle(mesh,material,{b,d,c});
      addTriangle(mesh,material,{a,c,b});
      addTriangle(mesh,material,{b,c,d});
      }
  return first;
  }

struct ClothWorld {
  asset::WorldMesh mesh;
  uint32_t city = 0, ground = 0;        // material indices
  uint32_t bannerVertex = 0;            // the first position of the city banner in cell 0,0
  uint32_t flagFirst = 0, flagEnd = 0;  // the ship flag's triangles
  };

// Cell -1,0: the unsupported quad. Cell 0,0: the sail, the ship flag, the rigid finial with the banner material (it
// reaches into cell 0,-1) and a city banner. A second city banner across the border of cells 0,0 and 1,0.
// Cell 2,0: a ground triangle. Cells -1,0, 0,-1 and 2,0 hold no cloth.
inline ClothWorld clothWorld() {
  ClothWorld w;
  auto& mesh = w.mesh;
  const auto other = addMaterial(mesh,"UNSUPPORTED_CLOTH","UNKNOWN_FLAG.TGA");
  const auto sail  = addMaterial(mesh,"MATERIAL_WITH_TEXTURE_RULE","nw_ship_sail_02.tga");
  const auto flag  = addMaterial(mesh,"nw_ship_flag_01","SYNTHETIC_FLAG.TGA");
  w.city   = addMaterial(mesh,"NW_CITY_FLAG_01","SYNTHETIC_CITY.TGA");
  w.ground = addMaterial(mesh,"CONTRACT_GROUND","CONTRACT_GROUND.TGA");
  mesh.materials[sail].wave_mode = asset::WaveMode::NONE;
  addBanner(mesh,other,-800);
  addGrid(mesh,sail,true);
  w.flagFirst = addGrid(mesh,flag,false);
  w.flagEnd = uint32_t(mesh.polygons.material_indices.size());
  addFinial(mesh,w.city);
  w.bannerVertex = addBanner(mesh,w.city,0);
  addBanner(mesh,w.city,6300);
  const auto base = uint32_t(mesh.vertices.size());
  mesh.vertices.insert(mesh.vertices.end(),{{13000,0,0},{13100,0,0},{13000,0,100}});
  addTriangle(mesh,w.ground,{base,base+1,base+2});
  return w;
  }

// A chunk GLB of one cell of the fixture as an edit writes it: the cell's triangles that keep accepts, with their
// orders, source ids and flags, each corner through move, then the added primitives.
inline void writeCell(const std::filesystem::path& path, const asset::WorldMesh& base, ChunkId cell,
                      const std::function<bool(size_t)>& keep, const std::function<void(GlbCorner&,uint32_t)>& move,
                      std::vector<GlbPrimitive> added={}) {
  const auto& p = base.polygons;
  std::vector<GlbPrimitive> primitives(base.materials.size());
  for(size_t m=0;m<primitives.size();++m) {
    primitives[m].material = base.materials[m];
    primitives[m].index = int64_t(m);
    }
  for(size_t t=0;t<p.material_indices.size();++t) {
    if(!(centroidCell(base,t)==cell) || !keep(t))
      continue;
    auto& primitive = primitives[p.material_indices[t]];
    std::array<GlbCorner,3> corners;
    for(unsigned c=0;c<3;++c) {
      const auto v = p.vertex_indices[t*3+c], f = p.feature_indices[t*3+c];
      corners[c] = {base.vertices[v],base.features[f],float(v),float(f)};
      move(corners[c],v);
      }
    primitive.triangles.push_back(corners);
    primitive.order.push_back(float(t));
    primitive.flags.push_back(wcb::packFlags(p.flags[t]));
    primitive.flags.push_back(uint16_t(p.flags[t].sector_index));
    primitive.lightmap.push_back(float(p.lightmap_indices[t]));
    }
  std::vector<GlbPrimitive> used;
  for(auto& primitive:primitives)
    if(!primitive.triangles.empty())
      used.push_back(std::move(primitive));
  for(auto& primitive:added)
    used.push_back(std::move(primitive));
  std::filesystem::create_directories(path.parent_path());
  writeChunkGlb(path,used);
  }

// The override sets, each written into dir. Cell 0,0 with a lower corner of its city banner moved:
inline void writeMovedClothCell(const std::filesystem::path& dir, const ClothWorld& world) {
  writeCell(dir/"chunks/0_0.glb",world.mesh,{0,0},[](size_t) { return true; },[&](GlbCorner& c, uint32_t v) {
    if(v==world.bannerVertex+2)
      c.position.x += 10.f;
    });
  }

// Cell 2,0 with a city banner as new geometry: new positions, no source ids or orders.
inline void writeAddedClothCell(const std::filesystem::path& dir, const ClothWorld& world) {
  GlbPrimitive banner;
  banner.material = world.mesh.materials[world.city];
  banner.index = int64_t(world.city);
  const asset::Vec3 corners[4] = {{13200,600,0},{13400,600,0},{13400,300,0},{13200,300,0}};
  for(const auto& tri:std::array<std::array<uint32_t,3>,4>{{{0,1,2},{0,2,3},{0,2,1},{0,3,2}}}) {
    std::array<GlbCorner,3> t;
    for(unsigned c=0;c<3;++c)
      t[c] = {corners[tri[c]],{{float(tri[c])*.25f,.5f},0xFF405060u,{0.f,0.f,1.f}},-1.f,-1.f};
    banner.triangles.push_back(t);
    banner.flags.insert(banner.flags.end(),{0,0});
    banner.lightmap.push_back(-1.f);
    }
  writeCell(dir/"chunks/2_0.glb",world.mesh,{2,0},[](size_t) { return true; },[](GlbCorner&,uint32_t) {},{banner});
  }

// Cell 0,0 without the ship flag's triangles.
inline void writeRemovedClothCell(const std::filesystem::path& dir, const ClothWorld& world) {
  writeCell(dir/"chunks/0_0.glb",world.mesh,{0,0},[&](size_t t) { return t<world.flagFirst || t>=world.flagEnd; },
            [](GlbCorner&,uint32_t) {});
  }

// ---- the base's worlds in a scratch copy ----

inline size_t cookedFiles(const std::filesystem::path& dir) {
  size_t count=0;
  for(const auto& entry:std::filesystem::recursive_directory_iterator(dir)) {
    const auto name = entry.path().filename();
    count += entry.path().extension()==".wcb" || name==wci::FileName || name==wcl::FileName;
    }
  return count;
  }

// Hard links (or copies across file systems) of every file of a world, without cooked files.
inline void linkWorld(const std::filesystem::path& from, const std::filesystem::path& to) {
  namespace fs = std::filesystem;
  fs::create_directories(to);
  for(const auto& entry:fs::recursive_directory_iterator(from)) {
    const auto target = to/fs::relative(entry.path(),from);
    if(entry.is_directory()) {
      fs::create_directories(target);
      continue;
      }
    const auto name = entry.path().filename();
    if(!entry.is_regular_file() || entry.path().extension()==".wcb" || name==wci::FileName || name==wcl::FileName)
      continue;
    std::error_code error;
    fs::create_hard_link(entry.path(),target,error);
    if(error)
      fs::copy_file(entry.path(),target);
    }
  }

}

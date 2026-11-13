// Cooked binary chunks (.wcb): cook -> assemble equals readWorldChunks for the
// world base contract's fixtures, an edited override and the base's NEWWORLD;
// malformed and stale files are rejected; the override question of
// docs/landscape-plan.md (Д3, task 2) is answered; and the manifest cases
// carried over from task 1. Needs GOTHIC_DIR and a ready base for NEWWORLD.

#include "commandline.h"
#include "gothicglb.h"
#include "resources.h"
#include "worldchunkcook.h"
#include "worldchunk_fixtures.h"
#include "worldchunks.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;
using namespace WorldChunkFixtures;

void require(bool condition, const std::string& message) {
  if(!condition)
    throw std::runtime_error(message);
  }

struct TemporaryDirectory {
  fs::path path = fs::temp_directory_path()/
    ("gothic-wcb-contract-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  ~TemporaryDirectory() {
    std::error_code error;
    fs::remove_all(path,error);
    }
  };

const WorldChunkSource fixtureSource = {"CONTRACT.ZEN",3,0x00C0FFEEu};

CookedWorldStats cookedRoundTrip(const fs::path& dir, const fs::path& over, const fs::path& out, const std::string& label) {
  const auto stats=cookWorldChunks(dir,over,out);
  require(stats.cells==readWorldChunkManifest(dir).cells.size(),label+": one cooked chunk per cell");
  const auto difference=verifyCookedWorld(dir,over,out,true);
  require(difference.empty(),label+": cook -> assemble differs from readWorldChunks: "+difference);
  std::printf("%s: %zu cells, %llu triangles, %llu GLB -> %llu .wcb bytes; cook -> assemble equals readWorldChunks\n",
              label.c_str(),stats.cells,static_cast<unsigned long long>(stats.triangles),
              static_cast<unsigned long long>(stats.glbBytes),static_cast<unsigned long long>(stats.wcbBytes));
  return stats;
  }

void expectThrows(const std::function<void()>& call, const std::vector<std::string>& parts, const std::string& what) {
  try {
    call();
    }
  catch(const std::runtime_error& error) {
    const std::string message=error.what();
    for(const auto& part:parts)
      require(message.find(part)!=std::string::npos,what+": the error must say '"+part+"', got: "+message);
    return;
    }
  throw std::runtime_error(what+" must be rejected");
  }

// ---- the world base contract's fixtures ----

void selectionFixtures(const fs::path& root) {
  const auto original=selectionMesh();
  const auto dir=root/"selection";
  const auto manifest=writeWorldChunks(original,fixtureSource,dir);
  cookedRoundTrip(dir,{},root/"selection-cooked","selection");
  auto changed=original;
  changed.vertices[0].x+=1.f;
  writeWorldChunks(changed,fixtureSource,root/"selection-changed");
  const auto over=root/"selection-override";
  fs::create_directories(over/"chunks");
  fs::copy_file(root/"selection-changed"/manifest.chunks.front(),over/manifest.chunks.front());
  cookedRoundTrip(dir,over,root/"selection-changed-cooked","selection, changed override");
  auto moved=original;
  for(auto& vertex:moved.vertices)
    vertex.x+=4.f*ChunkId::Size;
  const auto movedManifest=writeWorldChunks(moved,fixtureSource,root/"selection-moved");
  fs::copy_file(root/"selection-moved"/movedManifest.chunks.front(),over/manifest.chunks.front(),
                fs::copy_options::overwrite_existing);
  cookedRoundTrip(dir,over,root/"selection-moved-cooked","selection, moved override");
  }

std::string drawMesh(const std::string& file, const std::string& level="0") {
  return "{\"file\":\""+file+"\",\"role\":\"draw\",\"level\":"+level+"}";
  }

std::string cellEntry(const std::string& id, const std::string& meshes) {
  return "{\"cell\":"+id+",\"meshes\":["+meshes+"]}";
  }

// The chunk fixture as strings, and as objects whose extra meshes exist on disk
// and are not GLBs: neither the reader nor the cook opens them.
void chunkFixtures(const fs::path& root) {
  const auto dir=root/"chunks";
  writeWorldChunks(chunkFixtureMesh(),fixtureSource,dir);
  cookedRoundTrip(dir,{},root/"chunks-cooked","chunk fixture");

  const auto objects=root/"chunk-objects";
  fs::copy(dir,objects,fs::copy_options::recursive);
  const auto cells=readWorldChunkManifest(dir).cells;
  std::string list;
  for(size_t i=0;i<cells.size();++i) {
    const auto stem="chunks/"+std::to_string(cells[i].id.x)+"_"+std::to_string(cells[i].id.z);
    for(const auto* extra:{"_collision.glb","_lod1.glb"})
      writeText(objects/(stem+extra),"not a GLB: never opened");
    list+=std::string(i==0 ? "" : ",")+cellEntry("["+std::to_string(cells[i].id.x)+","+std::to_string(cells[i].id.z)+"]",
          "{\"file\":\""+stem+"_collision.glb\",\"role\":\"collision\"},"+drawMesh(stem+"_lod1.glb","1")+","+
          drawMesh(cells[i].drawFile()));
    }
  writeText(objects/"world.json",withChunks(readText(dir/"world.json"),list));
  ChunkTable strings,table;
  const auto expected=readWorldChunks(dir,{},&strings);
  const auto actual=readWorldChunks(objects,{},&table);
  auto difference=compareWorldMeshes(expected,actual);
  require(difference.empty(),"object entries with extra files on disk must read the same mesh: "+difference);
  require(table.entries.size()==strings.entries.size(),"object entries must give the same table");
  for(size_t i=0;i<table.entries.size();++i) {
    require(table.entries[i].meshes.size()==3,"object entries must carry their three meshes");
    table.entries[i].meshes=strings.entries[i].meshes;
    }
  difference=compareChunkTables(strings,table);
  require(difference.empty(),"object entries with extra files on disk must give the same table: "+difference);
  cookedRoundTrip(objects,{},root/"chunk-objects-cooked","chunk fixture as objects with unreadable extra files");
  }

// ---- an edited override: moves, splits and adds vertices ----

// A strip of 12 quads over cells 0,0 / 1,0 / 2,0: columns every 16 m, two rows.
uint32_t stripVertex(uint32_t column, uint32_t row) {
  return column*2+row;
  }

asset::WorldMesh stripMesh() {
  asset::WorldMesh mesh;
  mesh.materials.resize(2);
  mesh.materials[0].name="STRIP_GROUND";
  mesh.materials[0].group=asset::MaterialGroup::EARTH;
  mesh.materials[0].texture="STRIP_GROUND.TGA";
  mesh.materials[1].name="STRIP_ROCK";
  mesh.materials[1].group=asset::MaterialGroup::STONE;
  mesh.materials[1].detail_object="STRIP_DETAIL";
  for(uint32_t column=0;column<=12;++column)
    for(uint32_t row=0;row<2;++row) {
      mesh.vertices.push_back({float(column)*1600.f,float(column)+float(row)*7.f,float(row)*1600.f+100.f});
      mesh.features.push_back({{float(column),float(row)},0xFF000000u|(column*0x010203u),{0.f,1.f,-0.f}});
      }
  // Quad 4, the first of cell 1 at the border to cell 0, holds the lowest orders.
  for(const uint32_t quad:{4u,0u,1u,2u,3u,5u,6u,7u,8u,9u,10u,11u}) {
    const std::array<std::array<uint32_t,3>,2> triangles = {{
      {stripVertex(quad,0),stripVertex(quad+1,0),stripVertex(quad+1,1)},
      {stripVertex(quad,0),stripVertex(quad+1,1),stripVertex(quad,1)}}};
    for(const auto& triangle:triangles) {
      mesh.polygons.vertex_indices.insert(mesh.polygons.vertex_indices.end(),triangle.begin(),triangle.end());
      mesh.polygons.feature_indices.insert(mesh.polygons.feature_indices.end(),triangle.begin(),triangle.end());
      mesh.polygons.material_indices.push_back(quad%3==0 ? 1u : 0u);
      mesh.polygons.lightmap_indices.push_back(quad%2==0 ? -1 : int32_t(quad));
      mesh.polygons.flags.push_back(wcb::unpackFlags(uint16_t(0x208u|(quad&1u)),int16_t(int32_t(quad)-6)));
      }
    }
  mesh.vertices.push_back({1.f,2.f,3.f}); // referenced by nothing: unreferenced.bin
  mesh.features.push_back({{0.5f,0.5f},0x80FFFFFFu,{1.f,0.f,0.f}});
  return mesh;
  }

struct GlbCorner {
  asset::Vec3          position;
  asset::VertexFeature feature;
  float                vertexId, featureId; // -1 for geometry an edit added
  };

struct GlbPrimitive {
  asset::Material                      material;
  int64_t                              index = -1; // source material index; -1 for a new material
  std::vector<std::array<GlbCorner,3>> triangles;
  std::vector<float>                   order;      // empty: no order, as for geometry an edit added
  std::vector<uint16_t>                flags;      // two per triangle
  std::vector<float>                   lightmap;
  };

// A chunk GLB as an edit writes it: non-indexed primitives, the converter's 0.01 node scale.
void writeChunkGlb(const fs::path& path, const std::vector<GlbPrimitive>& primitives) {
  gothicglb::GlbBuilder glb;
  std::string materials="[", list="[";
  for(size_t i=0;i<primitives.size();++i) {
    const auto& p=primitives[i];
    std::vector<float> position,normal,uv,vid,fid;
    std::vector<uint8_t> color;
    std::array<float,3> lo,hi;
    lo.fill(std::numeric_limits<float>::infinity());
    hi.fill(-std::numeric_limits<float>::infinity());
    for(const auto& triangle:p.triangles)
      for(const auto& c:triangle) {
        const std::array<float,3> stored={-c.position.x,c.position.y,c.position.z};
        for(unsigned axis=0;axis<3;++axis) {
          position.push_back(stored[axis]);
          lo[axis]=std::min(lo[axis],stored[axis]);
          hi[axis]=std::max(hi[axis],stored[axis]);
          }
        normal.insert(normal.end(),{-c.feature.normal.x,c.feature.normal.y,c.feature.normal.z});
        uv.insert(uv.end(),{c.feature.texture.x,c.feature.texture.y});
        const auto light=c.feature.light;
        color.insert(color.end(),{uint8_t(light>>16),uint8_t(light>>8),uint8_t(light),uint8_t(light>>24)});
        vid.push_back(c.vertexId);
        fid.push_back(c.featureId);
        }
    const auto minMax=",\"min\":["+gothicglb::jsonFloat(lo[0])+","+gothicglb::jsonFloat(lo[1])+","+gothicglb::jsonFloat(lo[2])+
                      "],\"max\":["+gothicglb::jsonFloat(hi[0])+","+gothicglb::jsonFloat(hi[1])+","+gothicglb::jsonFloat(hi[2])+"]";
    const auto aPos=glb.accessor(position,5126,"VEC3",3,false,34962,minMax);
    const auto aNrm=glb.accessor(normal,5126,"VEC3",3,false,34962);
    const auto aUv =glb.accessor(uv,5126,"VEC2",2,false,34962);
    const auto aCol=glb.accessor(color,5121,"VEC4",4,true,34962);
    const auto aVid=glb.accessor(vid,5126,"SCALAR",1,false,34962);
    const auto aFid=glb.accessor(fid,5126,"SCALAR",1,false,34962);
    std::string extras="{\"flags\":"+std::to_string(glb.accessor(p.flags,5123,"VEC2",2,false,0))+
                       ",\"lightmap\":"+std::to_string(glb.accessor(p.lightmap,5126,"SCALAR",1,false,0));
    if(!p.order.empty())
      extras+=",\"order\":"+std::to_string(glb.accessor(p.order,5126,"SCALAR",1,false,0));
    extras+="}";
    auto record=gothicglb::materialJson(p.material,p.index<0 ? 0 : size_t(p.index));
    if(p.index<0)
      record="{"+record.substr(record.find(',')+1); // no "index": a material the edit added
    const std::string comma=i==0 ? "" : ",";
    materials+=comma+"{\"name\":"+gothicglb::jsonString(p.material.name)+",\"extras\":{\"gothic\":"+record+"}}";
    list+=comma+"{\"attributes\":{\"POSITION\":"+std::to_string(aPos)+",\"NORMAL\":"+std::to_string(aNrm)+
          ",\"TEXCOORD_0\":"+std::to_string(aUv)+",\"COLOR_0\":"+std::to_string(aCol)+",\"_VERTEX\":"+std::to_string(aVid)+
          ",\"_FEATURE\":"+std::to_string(aFid)+"},\"material\":"+std::to_string(i)+",\"mode\":4,\"extras\":{\"gothic\":"+
          extras+"}}";
    }
  const auto json="{\"asset\":{\"version\":\"2.0\",\"generator\":\"gothic-wcb-contract\"},\"scene\":0,"
                  "\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"name\":\"edit\",\"mesh\":0,\"scale\":[0.01,0.01,0.01]}],"
                  "\"meshes\":[{\"primitives\":"+list+"]}],\"materials\":"+materials+"],\"accessors\":"+glb.accessorsJson()+
                  ",\"bufferViews\":"+glb.viewsJson()+",\"buffers\":[{\"byteLength\":"+std::to_string(glb.bin.size())+"}]}";
  const auto bytes=glb.finish(json);
  gothicglb::writeFile(path,bytes.data(),bytes.size());
  }

ChunkId centroidCell(const asset::WorldMesh& mesh, size_t triangle) {
  double x=0, z=0;
  for(unsigned c=0;c<3;++c) {
    const auto& v=mesh.vertices[mesh.polygons.vertex_indices[triangle*3+c]];
    x+=v.x;
    z+=v.z;
    }
  return ChunkId::fromPosition(float(x/3.),float(z/3.));
  }

// Cell 1,0 edited: an inner vertex moved, its last source triangle (order 15)
// replaced by two around a new vertex with a material the edit added, and,
// with moveBorder, the vertex it shares with cell 0,0 moved too, which splits it.
fs::path writeStripOverride(const fs::path& over, const asset::WorldMesh& base, bool moveBorder) {
  fs::create_directories(over/"chunks");
  const auto& p=base.polygons;
  auto corner=[&](size_t t, unsigned c) {
    const auto v=p.vertex_indices[t*3+c], f=p.feature_indices[t*3+c];
    GlbCorner r {base.vertices[v],base.features[f],float(v),float(f)};
    if(v==stripVertex(6,1))
      r.position.y+=50.f;
    if(moveBorder && v==stripVertex(4,0))
      r.position.y+=30.f;
    return r;
    };
  std::vector<GlbPrimitive> primitives(3);
  for(uint32_t m=0;m<2;++m) {
    primitives[m].material=base.materials[m];
    primitives[m].index=m;
    }
  constexpr size_t dropped=15;
  for(size_t t=0;t<p.material_indices.size();++t) {
    if(!(centroidCell(base,t)==ChunkId{1,0}) || t==dropped)
      continue;
    auto& primitive=primitives[p.material_indices[t]];
    primitive.triangles.push_back({corner(t,0),corner(t,1),corner(t,2)});
    primitive.order.push_back(float(t));
    primitive.flags.push_back(wcb::packFlags(p.flags[t]));
    primitive.flags.push_back(uint16_t(p.flags[t].sector_index));
    primitive.lightmap.push_back(float(p.lightmap_indices[t]));
    }
  auto& added=primitives[2];
  added.material=base.materials[0];
  added.material.name="STRIP_EDIT";
  added.material.group=asset::MaterialGroup::WOOD;
  const auto a=corner(dropped,0), b=corner(dropped,1), c=corner(dropped,2);
  GlbCorner middle {{(a.position.x+b.position.x)*0.5f,(a.position.y+b.position.y)*0.5f+20.f,(a.position.z+b.position.z)*0.5f},
                    {{0.25f,0.75f},0xFF406080u,{0.f,0.8f,0.6f}},-1.f,-1.f};
  added.triangles={{a,middle,c},{middle,b,c}};
  added.flags={0,0,0,0};
  added.lightmap={-1.f,-1.f};
  writeChunkGlb(over/"chunks/1_0.glb",primitives);
  return over;
  }

// The GLB order of each cooked triangle, against the base triangle with the
// same corner ids (its index is its order); added counts those without one.
void checkOrders(const CookedChunk& chunk, const asset::WorldMesh& base, size_t added, const std::string& label) {
  size_t without=0;
  const auto& vi=base.polygons.vertex_indices;
  for(const auto& t:chunk.triangles()) {
    const std::array<uint32_t,3> ids={chunk.positions()[t.vertex[0]].id,chunk.positions()[t.vertex[1]].id,
                                      chunk.positions()[t.vertex[2]].id};
    size_t order=0;
    while(order<base.polygons.material_indices.size() &&
          !(vi[order*3]==ids[0] && vi[order*3+1]==ids[1] && vi[order*3+2]==ids[2]))
      ++order;
    if(order==base.polygons.material_indices.size()) {
      require(t.order==wcb::Added,label+": a triangle an edit added must carry the added order");
      ++without;
      continue;
      }
    require(t.order==order,label+": a source triangle must carry its order "+std::to_string(order)+", got "+
            std::to_string(t.order));
    }
  require(without==added,label+": "+std::to_string(without)+" added triangles, expected "+std::to_string(added));
  }

// Fields in which two cooked chunks of one cell differ.
std::set<std::string> differingFields(const CookedChunk& a, const CookedChunk& b) {
  std::set<std::string> out;
  const auto& x=a.header();
  const auto& y=b.header();
  if(x.vertices!=y.vertices || x.features!=y.features || x.triangles!=y.triangles || x.materials!=y.materials)
    out.insert("header.counts");
  if(!(x.min==y.min) || !(x.max==y.max))
    out.insert("header.bounds");
  if(x.glbSize!=y.glbSize || x.glbSha256!=y.glbSha256)
    out.insert("header.glb");
  const auto bits=[](float v) { return std::bit_cast<uint32_t>(v); };
  for(size_t i=0;i<std::min(a.positions().size(),b.positions().size());++i) {
    const auto& p=a.positions()[i];
    const auto& q=b.positions()[i];
    if(bits(p.x)!=bits(q.x) || bits(p.y)!=bits(q.y) || bits(p.z)!=bits(q.z))
      out.insert("positions.value");
    if(p.id!=q.id)
      out.insert("positions.id");
    }
  for(size_t i=0;i<std::min(a.features().size(),b.features().size());++i) {
    const auto& p=a.features()[i];
    const auto& q=b.features()[i];
    if(bits(p.u)!=bits(q.u) || bits(p.v)!=bits(q.v) || p.light!=q.light || bits(p.nx)!=bits(q.nx) ||
       bits(p.ny)!=bits(q.ny) || bits(p.nz)!=bits(q.nz))
      out.insert("features.value");
    if(p.id!=q.id)
      out.insert("features.id");
    }
  for(size_t i=0;i<std::min(a.triangles().size(),b.triangles().size());++i) {
    const auto& p=a.triangles()[i];
    const auto& q=b.triangles()[i];
    for(unsigned c=0;c<3;++c) {
      if(p.vertex[c]!=q.vertex[c])
        out.insert("triangles.vertex");
      if(p.feature[c]!=q.feature[c])
        out.insert("triangles.feature");
      }
    if(p.material!=q.material) out.insert("triangles.material");
    if(p.order!=q.order)       out.insert("triangles.order");
    if(p.rank!=q.rank)         out.insert("triangles.rank");
    if(p.lightmap!=q.lightmap || p.flags!=q.flags || p.sector!=q.sector)
      out.insert("triangles.lightmap/flags/sector");
    }
  for(size_t i=0;i<std::min(a.materials().size(),b.materials().size());++i)
    if(a.materials()[i].id!=b.materials()[i].id || !gothicglb::sameMaterial(a.material(i),b.material(i)))
      out.insert("materials");
  return out;
  }

std::string joined(const std::set<std::string>& fields) {
  std::string out;
  for(const auto& field:fields)
    out+=(out.empty() ? "" : ", ")+field;
  return out.empty() ? "nothing" : out;
  }

void editedOverride(const fs::path& root) {
  const auto base=stripMesh();
  const auto dir=root/"strip";
  const auto manifest=writeWorldChunks(base,fixtureSource,dir);
  require(manifest.chunks==std::vector<std::string>{"chunks/0_0.glb","chunks/1_0.glb","chunks/2_0.glb"},
          "the strip fixture must occupy cells 0,0 / 1,0 / 2,0");
  const auto plain=root/"strip-cooked";
  cookedRoundTrip(dir,{},plain,"strip");

  const auto edit=writeStripOverride(root/"strip-edit",base,false);
  const auto border=writeStripOverride(root/"strip-border",base,true);
  ChunkTable table;
  const auto edited=readWorldChunks(dir,border,&table);
  require(edited.vertices.size()==base.vertices.size()+2 && edited.features.size()==base.features.size()+1 &&
          edited.materials.size()==3 && edited.polygons.material_indices.size()==base.polygons.material_indices.size()+1,
          "the edited override must split the border vertex, add one vertex, one feature, one material and one net triangle");
  cookedRoundTrip(dir,edit,root/"strip-edit-cooked","strip, override that moves an inner vertex and splits a triangle");
  cookedRoundTrip(dir,border,root/"strip-border-cooked","strip, override that also moves a border vertex");
  for(const auto* cell:{"0_0","1_0","2_0"})
    checkOrders(CookedChunk::read(plain/"chunks"/(std::string(cell)+".wcb")),base,0,std::string("strip ")+cell);
  checkOrders(CookedChunk::read(root/"strip-edit-cooked/chunks/1_0.wcb"),base,2,"edited 1_0");

  // Without --out the cooked chunks lie beside the GLB each cell is read from.
  const auto beside=root/"strip-beside";
  fs::copy(dir,beside,fs::copy_options::recursive);
  const auto besideOver=root/"strip-beside-override";
  fs::copy(border,besideOver,fs::copy_options::recursive);
  cookWorldChunks(beside,besideOver);
  require(fs::exists(beside/"chunks/0_0.wcb") && fs::exists(beside/"chunks/2_0.wcb") &&
          fs::exists(besideOver/"chunks/1_0.wcb") && !fs::exists(beside/"chunks/1_0.wcb"),
          "a cell read from the override must have its .wcb beside the override GLB, the others beside theirs");
  require(verifyCookedWorld(beside,besideOver,{},true).empty(),"cooked chunks beside their GLBs must assemble");

  // The override question: do the cooked records of the cells the override does
  // not replace differ from the records cooked without it?
  struct Answer { std::string override, cell; std::set<std::string> expected; };
  const std::vector<Answer> answers = {
    {"strip-edit-cooked","0_0",{}},
    {"strip-edit-cooked","2_0",{"triangles.rank"}},
    {"strip-border-cooked","0_0",{"positions.id"}},
    {"strip-border-cooked","2_0",{"triangles.rank"}},
    };
  for(const auto& answer:answers) {
    const auto file="chunks/"+answer.cell+".wcb";
    const auto fields=differingFields(CookedChunk::read(plain/file),CookedChunk::read(root/answer.override/file));
    std::printf("Override question: %s, cell %s not replaced, differs from its cook without the override in: %s\n",
                answer.override.c_str(),answer.cell.c_str(),joined(fields).c_str());
    require(fields==answer.expected,"override question: cell "+answer.cell+" of "+answer.override+" differs in "+
            joined(fields)+", expected "+joined(answer.expected));
    }
  }

// ---- malformed and stale cooked chunks ----

uint32_t get32(const std::vector<uint8_t>& bytes, size_t at) {
  uint32_t value=0;
  for(unsigned i=0;i<4;++i)
    value|=uint32_t(bytes[at+i])<<(8*i);
  return value;
  }

void put32(std::vector<uint8_t>& bytes, size_t at, uint32_t value) {
  for(unsigned i=0;i<4;++i)
    bytes[at+i]=uint8_t(value>>(8*i));
  }

void put64(std::vector<uint8_t>& bytes, size_t at, uint64_t value) {
  for(unsigned i=0;i<8;++i)
    bytes[at+i]=uint8_t(value>>(8*i));
  }

void rejections(const fs::path& root) {
  const auto dir=root/"stale";
  fs::copy(root/"chunks",dir,fs::copy_options::recursive);
  cookWorldChunks(dir);
  const auto manifest=readWorldChunkManifest(dir);
  const auto file=fs::path(manifest.chunks.front());
  auto wcbFile=dir/file;
  wcbFile.replace_extension(".wcb");
  const auto good=gothicglb::readFile(wcbFile);
  const auto open=[](const std::vector<uint8_t>& bytes) {
    return [bytes] { (void)CookedChunk::open(bytes,"fixture.wcb"); };
    };
  (void)CookedChunk::open(good,"fixture.wcb");
  constexpr size_t directory=wcb::HeaderSize, entry=32;
  constexpr size_t positions=directory, triangles=directory+2*entry, strings=directory+4*entry;

  auto bytes=good;
  bytes.pop_back();
  expectThrows(open(bytes),{"fixture.wcb","header","truncated"},"a truncated file");
  bytes.assign(good.begin(),good.begin()+100);
  expectThrows(open(bytes),{"fixture.wcb","header","truncated: 100 bytes"},"a file shorter than its header");
  bytes=good;
  bytes[0]='X';
  expectThrows(open(bytes),{"header","wrong magic"},"a wrong magic");
  bytes=good;
  put32(bytes,4,wcb::Version+1);
  expectThrows(open(bytes),{"header","version "+std::to_string(wcb::Version+1)},"another version");
  bytes=good;
  put64(bytes,triangles+16,(good.size()+31)/16*16);
  expectThrows(open(bytes),{"triangles","past the end of the file"},"a section past the end of the file");
  bytes=good;
  put32(bytes,strings+8,get32(good,strings+8)+1); // the last section: starts inside the file, ends one byte past it
  put64(bytes,strings+24,get32(good,strings+24)+1ull);
  expectThrows(open(bytes),{"strings","past the end of the file"},"a section that ends past the end of the file");
  bytes=good;
  put32(bytes,positions+8,get32(good,positions+8)+1);
  expectThrows(open(bytes),{"positions","records of 16 bytes do not fill"},"a count that disagrees with its section");
  bytes=good;
  put32(bytes,32,get32(good,32)+1);
  expectThrows(open(bytes),{"positions","the header counts"},"a header count that disagrees with its section");

  // The stored hash is SHA-256 itself (FIPS 180-4 vector "abc"), not only a function of the bytes.
  const std::string abc="abc";
  const std::array<uint8_t,32> abcSha={0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
                                       0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad};
  require(sha256Bytes({reinterpret_cast<const uint8_t*>(abc.data()),abc.size()})==abcSha,"sha256Bytes must be SHA-256");

  // A GLB that changed after cooking: by length (the cheap check) and, same length, by SHA-256.
  const auto glb=dir/file;
  auto glbBytes=gothicglb::readFile(glb);
  const auto original=glbBytes;
  glbBytes.push_back(0);
  gothicglb::writeFile(glb,glbBytes.data(),glbBytes.size());
  expectThrows([&] { (void)readCookedWorld(dir); },
               {wcbFile.filename().string(),"stale",std::to_string(glbBytes.size())+" bytes, the chunk was cooked from "+
                std::to_string(original.size())+" bytes"},"a .wcb whose GLB changed length");
  glbBytes=original;
  // The same length with one byte of the JSON generator name changed: the GLB still reads, its hash differs.
  const std::string generator="worldchunks";
  const auto at=std::search(glbBytes.begin(),glbBytes.end(),generator.begin(),generator.end());
  require(at!=glbBytes.end(),"the fixture GLB names its generator");
  *at='W';
  gothicglb::writeFile(glb,glbBytes.data(),glbBytes.size());
  const auto chunk=CookedChunk::read(wcbFile);
  require(cookedChunkStale(chunk,glb,false).empty(),"a GLB of the same length passes the length check");
  require(cookedChunkStale(chunk,glb,true).find("another SHA-256")!=std::string::npos,
          "a GLB with other bytes of the same length fails the hash check");
  require(verifyCookedWorld(dir,{},{},true).find("another SHA-256")!=std::string::npos,
          "verify with hashes must reject a GLB with other bytes");
  gothicglb::writeFile(glb,original.data(),original.size());
  require(verifyCookedWorld(dir,{},{},true).empty(),"the restored GLB matches its cooked chunk again");
  std::puts("Rejections: truncated, short, wrong magic, version, a section starting and one ending past the end, "
            "two count mismatches, GLB of another length and of another SHA-256; SHA-256 vector");
  }

// ---- manifest cases carried over from task 1 ----

void manifestCases(const fs::path& root) {
  const auto json=readText(root/"chunks"/"world.json");
  const auto dir=root/"manifest-rejected";
  fs::create_directories(dir);
  const auto rejected=[&](const std::string& list, const std::vector<std::string>& parts) {
    expectThrows([&] {
      writeText(dir/"world.json",withChunks(json,list));
      (void)readWorldChunkManifest(dir);
      },parts,"manifest "+list);
    };
  const auto zero=drawMesh("chunks/0_0.glb");
  rejected(cellEntry("[0,0]",zero+","+drawMesh("chunks/0_0_a.glb","1")+","+drawMesh("chunks/0_0_b.glb","1")),
           {"two draw level 1 meshes, chunks/0_0_a.glb and chunks/0_0_b.glb"});
  rejected(cellEntry("[0,0]",drawMesh("")),{"chunk path must be relative: "});
  const auto own=drawMesh("chunks/3_-2.glb");
  rejected("\"chunks/0_0.glb\","+cellEntry("[3,-2]",own+",{\"file\":\"chunks/3_-2_s.glb\",\"role\":\"shadow\"}"),
           {"chunks entry 1, cell 3,-2","unknown role 'shadow' of chunks/3_-2_s.glb"});
  rejected("\"chunks/0_0.glb\","+cellEntry("[3,-2]",own+",{\"file\":\"chunks/3_-2_c.glb\",\"role\":\"collision\"},"
                                                         "{\"file\":\"chunks/3_-2_d.glb\",\"role\":\"collision\"}"),
           {"chunks entry 1, cell 3,-2","two collision meshes, chunks/3_-2_c.glb and chunks/3_-2_d.glb"});
  rejected("\"chunks/0_0.glb\","+cellEntry("[3,-2]",own+","+drawMesh("chunks/3_-2_x.glb")),
           {"chunks entry 1, cell 3,-2","two draw level 0 meshes, chunks/3_-2.glb and chunks/3_-2_x.glb"});
  std::puts("Manifest: two draw level 1 meshes, an empty draw file, and unknown role and doubled meshes "
            "with their entry, cell and files rejected; extra files on disk are never opened");
  }

}

int main() {
  TemporaryDirectory temporary;
  try {
    fs::create_directories(temporary.path);
    selectionFixtures(temporary.path);
    chunkFixtures(temporary.path);
    editedOverride(temporary.path);
    rejections(temporary.path);
    manifestCases(temporary.path);

    const auto* gothic=std::getenv("GOTHIC_DIR");
    require(gothic!=nullptr && *gothic!='\0',"GOTHIC_DIR must point to a Gothic II installation");
    const char* args[]={"gothic-wcb-contract","-g",gothic};
    CommandLine commandLine(3,args);
    Resources resources;
    Resources::loadBase();
    const auto world=Resources::worldBaseDir("NEWWORLD.ZEN");
    require(!world.empty() && fs::exists(world/"world.json"),"the base has no NEWWORLD");
    cookedRoundTrip(world,{},temporary.path/"NEWWORLD-cooked","base NEWWORLD ("+world.string()+")");
    std::puts("Cooked chunk contract passed");
    return EXIT_SUCCESS;
    }
  catch(const std::exception& error) {
    std::fprintf(stderr,"FAIL: %s\n",error.what());
    return EXIT_FAILURE;
    }
  }

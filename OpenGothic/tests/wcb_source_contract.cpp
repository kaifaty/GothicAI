// The game's per-cell reader on cooked chunks (docs/landscape-plan.md, Д3,
// task 3; worldchunkcookedsource.h): ChunkMeshSource::read takes a cell from
// its .wcb beside the GLB and the set's index when they are there and valid,
// else from the GLB, and both give the same result.
//   - For the cooked chunk contract's fixtures, the strip with each override
//     and with two overridden cells, and the base's NEWWORLD cooked into a
//     scratch copy (hard links; the base is never written): every cell, read
//     on the cooked path, equals its read with OPENGOTHIC_WCB=0 field by field,
//     for the cell's full triangle list and for every third triangle reversed,
//     and the counters show which path ran.
//   - Every fallback gives the GLB result and its log line.
//   - Two threads reading at once give the single-thread result.
//   - The cook rewrites a .wcb whose bytes changed at the same length.
// Needs GOTHIC_DIR and a ready base for NEWWORLD.

#include "commandline.h"
#include "gothicglb.h"
#include "resources.h"
#include "worldchunkcook.h"
#include "worldchunk_fixtures.h"
#include "worldchunk_strip.h"
#include "worldchunks.h"

#include <Tempest/Log>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
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
    ("gothic-wcb-source-contract-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  ~TemporaryDirectory() {
    std::error_code error;
    fs::remove_all(path,error);
    }
  };

const WorldChunkSource fixtureSource = {"CONTRACT.ZEN",3,0x00C0FFEEu};

void setCookedPath(bool on) {
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

// The lines of the cooked reader in a capture: each starts with its tag.
size_t readerLines(const std::string& log) {
  size_t count=0;
  for(auto at=log.find("[world-chunks] ");at!=std::string::npos;at=log.find("[world-chunks] ",at+1))
    ++count;
  return count;
  }

std::vector<uint8_t> fileBytes(const fs::path& path) {
  return gothicglb::readFile(path);
  }

void writeBytes(const fs::path& path, const std::vector<uint8_t>& bytes) {
  gothicglb::writeFile(path,bytes.data(),bytes.size());
  }

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

std::string compareReads(const WorldChunkMesh& expected, const WorldChunkMesh& actual) {
  auto difference=compareWorldMeshes(expected.mesh,actual.mesh);
  if(!difference.empty())
    return difference;
  if(expected.vertexIds!=actual.vertexIds)
    return "vertexIds differ";
  if(expected.featureIds!=actual.featureIds)
    return "featureIds differ";
  return {};
  }

// A world as the game loads it, with each cell's GLB ordinals in stitched order and every third of them reversed.
struct Loaded {
  ChunkTable                         table;
  std::vector<std::vector<uint32_t>> full, filtered;
  const ChunkMeshSource& source() const { return *table.source; }
  };

Loaded loadWorld(const fs::path& dir, const fs::path& over, bool cooked) {
  Loaded out;
  asset::WorldMesh mesh;
  setCookedPath(cooked);
  try {
    mesh=readWorldChunks(dir,over,&out.table);
    }
  catch(...) {
    setCookedPath(true);
    throw;
    }
  setCookedPath(true);
  for(const auto& list:chunkTriangleLists(mesh,out.table)) {
    auto& full=out.full.emplace_back();
    for(const auto triangle:list)
      full.push_back(out.table.triangleSources[triangle]);
    auto& filtered=out.filtered.emplace_back();
    for(size_t k=0;k<full.size();k+=3)
      filtered.push_back(full[k]);
    std::reverse(filtered.begin(),filtered.end());
    }
  return out;
  }

struct Outcome {
  ChunkReadCounts cooked, glb; // the counters of the source loaded with the cooked path, and with OPENGOTHIC_WCB=0
  std::string     log;
  size_t          cells = 0;
  std::vector<std::string> glbCells; // the cells the cooked source read from their GLB
  };

// Loads dir with over twice, with the cooked path and with OPENGOTHIC_WCB=0, and reads every cell from
// both with its full and its filtered list: the results must be equal.
Outcome compareSet(const std::string& label, const fs::path& dir, const fs::path& over={}) {
  LogCapture log;
  const auto glb=loadWorld(dir,over,false);
  const auto cooked=loadWorld(dir,over,true);
  const auto cells=glb.table.entries.size();
  std::vector<std::string> glbCells;
  for(size_t i=0;i<cells;++i) {
    const auto before=cooked.source().reads().glb;
    for(const auto* lists:{&glb.full,&glb.filtered}) {
      const auto& sources=(*lists)[i];
      const auto difference=compareReads(glb.source().read(i,sources),cooked.source().read(i,sources));
      require(difference.empty(),label+": cell "+glb.table.entries[i].file+", "+(lists==&glb.full ? "full" : "filtered")+
              " list: the cooked path reads another result than the GLB path: "+difference);
      }
    if(cooked.source().reads().glb!=before)
      glbCells.push_back(glb.table.entries[i].file);
    }
  return {cooked.source().reads(),glb.source().reads(),log.take(),cells,std::move(glbCells)};
  }

// cookedCells of the set's cells took the cooked path for both their reads, the rest the GLB path; the
// source with OPENGOTHIC_WCB=0 read every cell from its GLB; the reader logged lines lines, with reason;
// with glbCell, that cell is the one that took the GLB path.
void expect(const std::string& label, const Outcome& o, size_t cookedCells, size_t lines, const std::string& reason={},
            const std::string& glbCell={}) {
  std::printf("%s: %llu of %zu cells cooked; log: %s\n",label.c_str(),static_cast<unsigned long long>(o.cooked.cooked/2),
              o.cells,o.log.empty() ? "(none)" : o.log.c_str());
  require(o.cooked.cooked==2*cookedCells && o.cooked.glb==2*(o.cells-cookedCells),
          label+": "+std::to_string(o.cooked.cooked)+" cooked and "+std::to_string(o.cooked.glb)+" GLB reads, expected "+
          std::to_string(2*cookedCells)+" and "+std::to_string(2*(o.cells-cookedCells)));
  require(o.glb.cooked==0 && o.glb.glb==2*o.cells,label+": with OPENGOTHIC_WCB=0 every read must take the GLB path");
  require(readerLines(o.log)==lines,label+": the reader must log "+std::to_string(lines)+" lines, logged: "+o.log);
  require(reason.empty() || o.log.find(reason)!=std::string::npos,label+": the log must say '"+reason+"', got: "+o.log);
  require(glbCell.empty() || o.glbCells==std::vector<std::string>{glbCell},label+": only "+glbCell+
          " must take the GLB path");
  }

void expectAllCooked(const std::string& label, const fs::path& dir, const fs::path& over={}) {
  const auto outcome=compareSet(label,dir,over);
  expect(label,outcome,outcome.cells,1,"cells read from their .wcb");
  }

// ---- every fixture of the cooked chunk contract, cooked beside its GLBs ----

void fixtureSets(const fs::path& root) {
  const auto selection=root/"selection";
  const auto manifest=writeWorldChunks(selectionMesh(),fixtureSource,selection);
  cookWorldChunks(selection);
  expectAllCooked("selection",selection);
  // The override's 0_0.glb: one vertex changed, or the whole triangle moved four cells away.
  auto changed=selectionMesh();
  changed.vertices[0].x+=1.f;
  auto moved=selectionMesh();
  for(auto& vertex:moved.vertices)
    vertex.x+=4.f*ChunkId::Size;
  for(const auto& [name,mesh]:{std::pair<std::string,asset::WorldMesh>{"changed",changed},{"moved",moved}}) {
    const auto edited=root/("selection-"+name);
    const auto file=writeWorldChunks(mesh,fixtureSource,edited).chunks.front();
    const auto over=root/("selection-"+name+"-override");
    fs::create_directories(over/"chunks");
    fs::copy_file(edited/file,over/manifest.chunks.front());
    cookWorldChunks(selection,over);
    expectAllCooked("selection with the "+name+" override",selection,over);
    }

  const auto chunks=root/"chunks";
  writeWorldChunks(chunkFixtureMesh(),fixtureSource,chunks);
  const auto objects=root/"chunk-objects";
  fs::copy(chunks,objects,fs::copy_options::recursive);
  const auto cells=readWorldChunkManifest(chunks).cells;
  std::string list;
  for(size_t i=0;i<cells.size();++i) {
    const auto stem="chunks/"+std::to_string(cells[i].id.x)+"_"+std::to_string(cells[i].id.z);
    for(const auto* extra:{"_collision.glb","_lod1.glb"})
      writeText(objects/(stem+extra),"not a GLB: never opened");
    list+=std::string(i==0 ? "" : ",")+cellEntry("["+std::to_string(cells[i].id.x)+","+std::to_string(cells[i].id.z)+"]",
          "{\"file\":\""+stem+"_collision.glb\",\"role\":\"collision\"},"+drawMesh(stem+"_lod1.glb","1")+","+
          drawMesh(cells[i].drawFile()));
    }
  writeText(objects/"world.json",withChunks(readText(chunks/"world.json"),list));
  for(const auto* dir:{&chunks,&objects}) {
    cookWorldChunks(*dir);
    expectAllCooked(dir==&chunks ? "chunk fixture" : "chunk fixture as objects with unreadable extra files",*dir);
    }

  // The strip with each override of the contract, and with 1_0 and 2_0 both overridden.
  const auto base=stripMesh();
  const auto strip=root/"strip";
  writeWorldChunks(base,fixtureSource,strip);
  cookWorldChunks(strip);
  expectAllCooked("strip",strip);
  const auto edit=writeStripOverride(root/"strip-edit",base,false);
  const auto border=writeStripOverride(root/"strip-border",base,true);
  const auto second=writeStripOverride(root/"strip-second",base,false,{2,0});
  const auto both=root/"strip-both";
  fs::create_directories(both/"chunks");
  fs::copy_file(edit/"chunks/1_0.glb",both/"chunks/1_0.glb");
  fs::copy_file(second/"chunks/2_0.glb",both/"chunks/2_0.glb");
  for(const auto* over:{&edit,&border,&second,&both}) {
    cookWorldChunks(strip,*over);
    expectAllCooked("strip with the override "+over->filename().string(),strip,*over);
    }
  // The same GLBs as another world directory: without cell 0,0, the edited 1,0 its own chunk.
  const auto other=root/"strip-other";
  writeWorldChunks(base,fixtureSource,other);
  fs::remove(other/"chunks/0_0.glb");
  fs::copy_file(edit/"chunks/1_0.glb",other/"chunks/1_0.glb",fs::copy_options::overwrite_existing);
  writeText(other/"world.json",withChunks(readText(strip/"world.json"),"\"chunks/1_0.glb\",\"chunks/2_0.glb\""));
  cookWorldChunks(other);
  expectAllCooked("strip without cell 0,0 and the edited 1,0 as its own chunk",other);
  }

// ---- an override directory: its cells' .wcb beside its GLBs, its own index ----

void overrideDirectory(const fs::path& root) {
  const auto base=root/"override-base", over=root/"override-set";
  writeWorldChunks(stripMesh(),fixtureSource,base);
  writeStripOverride(over,stripMesh(),true);
  // The base cooked alone holds its own 1_0.wcb and index too: the set must not take them.
  cookWorldChunks(base);
  cookWorldChunks(base,over);
  require(fs::exists(base/"chunks/1_0.wcb") && fs::exists(over/"chunks/1_0.wcb") && fs::exists(base/wci::FileName) &&
          fs::exists(over/wci::FileName),"the base and the override set must each have their 1_0.wcb and index");
  expectAllCooked("override set: 1_0 from the override's .wcb and index, 0_0 and 2_0 from the base's .wcb",base,over);
  expectAllCooked("the same base without its override, from its own index",base);
  // Without the override's 1_0.wcb the cell reads its override GLB, not the base's 1_0.wcb.
  fs::remove(over/"chunks/1_0.wcb");
  expect("override set without the override's 1_0.wcb",compareSet("override set without 1_0.wcb",base,over),2,1,{},
         "chunks/1_0.glb");
  }

// ---- fallbacks: each case in a fresh copy of the strip cooked beside its GLBs ----

// A GLB with four more bytes of JSON whitespace: the same mesh from a file of another length.
std::vector<uint8_t> padJson(std::vector<uint8_t> glb) {
  const auto json=get32(glb,12);
  glb.insert(glb.begin()+20+json,4,uint8_t(' '));
  put32(glb,12,json+4);
  put32(glb,8,uint32_t(glb.size()));
  return glb;
  }

// Cell 1,0 of the strip as an edit that only gives its ground triangles a material of its own (no world.json
// index): one appended material, the same geometry and source ids.
void writeMaterialEdit(const fs::path& over, const asset::WorldMesh& base) {
  fs::create_directories(over/"chunks");
  const auto& p=base.polygons;
  std::vector<GlbPrimitive> primitives(base.materials.size());
  for(size_t m=0;m<primitives.size();++m) {
    primitives[m].material=base.materials[m];
    primitives[m].index=int64_t(m);
    }
  primitives[0].material.name="STRIP_GROUND_EDIT";
  primitives[0].index=-1;
  for(size_t t=0;t<p.material_indices.size();++t) {
    if(!(centroidCell(base,t)==ChunkId{1,0}))
      continue;
    auto corner=[&](unsigned c) {
      const auto v=p.vertex_indices[t*3+c], f=p.feature_indices[t*3+c];
      return GlbCorner{base.vertices[v],base.features[f],float(v),float(f)};
      };
    auto& primitive=primitives[p.material_indices[t]];
    primitive.triangles.push_back({corner(0),corner(1),corner(2)});
    primitive.order.push_back(float(t));
    primitive.flags.push_back(wcb::packFlags(p.flags[t]));
    primitive.flags.push_back(uint16_t(p.flags[t].sector_index));
    primitive.lightmap.push_back(float(p.lightmap_indices[t]));
    }
  writeChunkGlb(over/"chunks/1_0.glb",primitives);
  }

void fallbacks(const fs::path& root) {
  const auto cooked=root/"fallback-cooked";
  writeWorldChunks(stripMesh(),fixtureSource,cooked);
  cookWorldChunks(cooked);
  size_t number=0;
  auto copy=[&] {
    const auto dir=root/("fallback-"+std::to_string(++number));
    fs::copy(cooked,dir,fs::copy_options::recursive);
    return dir;
    };

  auto dir=copy();
  fs::remove(dir/wci::FileName);
  expect("no index",compareSet("no index",dir),0,0);

  dir=copy();
  auto index=fileBytes(dir/wci::FileName);
  put32(index,32,get32(index,32)^1u); // the source world's CRC32
  writeBytes(dir/wci::FileName,index);
  expect("an index of another world",compareSet("an index of another world",dir),0,1,
         "names another source world than CONTRACT.ZEN");

  dir=copy();
  index=fileBytes(dir/wci::FileName);
  put32(index,40,get32(index,40)+1); // the vertex total
  writeBytes(dir/wci::FileName,index);
  expect("an index with other totals",compareSet("an index with other totals",dir),0,1,
         "differ from the loaded world's");

  dir=copy();
  index=fileBytes(dir/wci::FileName);
  put32(index,44,get32(index,44)+1); // the feature total alone
  writeBytes(dir/wci::FileName,index);
  expect("an index with another feature total",compareSet("an index with another feature total",dir),0,1,
         "28 features, 2 materials and 24 triangles differ from the loaded world's 27, 27,");

  dir=copy();
  index=fileBytes(dir/wci::FileName);
  put32(index,52,get32(index,52)+1); // the triangle total alone
  writeBytes(dir/wci::FileName,index);
  expect("an index with another triangle total",compareSet("an index with another triangle total",dir),0,1,
         "2 materials and 25 triangles differ from the loaded world's 27, 27, 2 and 24");

  // Only the material total: an index that passes its own checks, cooked for a set whose 1_0 has an
  // appended material and the base's geometry, put beside the base without that override.
  dir=copy();
  const auto materialEdit=root/"fallback-material-edit";
  writeMaterialEdit(materialEdit,stripMesh());
  cookWorldChunks(dir,materialEdit);
  fs::copy_file(materialEdit/wci::FileName,dir/wci::FileName,fs::copy_options::overwrite_existing);
  expect("an index with another material total",compareSet("an index with another material total",dir),0,1,
         "27 vertices, 27 features, 3 materials and 24 triangles differ from the loaded world's 27, 27, 2 and 24");

  dir=copy();
  fs::remove(dir/"chunks/1_0.wcb");
  expect("a missing .wcb",compareSet("a missing .wcb",dir),2,1,{},"chunks/1_0.glb");

  dir=copy();
  auto chunk=fileBytes(dir/"chunks/1_0.wcb");
  chunk.pop_back();
  writeBytes(dir/"chunks/1_0.wcb",chunk);
  expect("a truncated .wcb",compareSet("a truncated .wcb",dir),2,2,"truncated","chunks/1_0.glb");

  dir=copy();
  fs::copy_file(dir/"chunks/0_0.wcb",dir/"chunks/1_0.wcb",fs::copy_options::overwrite_existing);
  expect("the .wcb of another cell",compareSet("the .wcb of another cell",dir),2,2,
         "was cooked for cell 0,0, world.json lists it for cell 1,0","chunks/1_0.glb");

  dir=copy();
  writeBytes(dir/"chunks/1_0.glb",padJson(fileBytes(dir/"chunks/1_0.glb")));
  expect("a GLB of another length after the cook",compareSet("a GLB of another length",dir),2,2,
         "bytes at world load, the index lists the cell with a GLB of","chunks/1_0.glb");

  // In a set with edits a GLB of another length rejects the whole index. Here 1_0's new edit moves the border
  // vertex it shares with 2_0 instead of the one it shares with 0_0: the totals stay, both neighbours' copies renumber.
  {
  const auto base=root/"fallback-edited", over=root/"fallback-edited-override";
  writeWorldChunks(stripMesh(),fixtureSource,base);
  writeStripOverride(over,stripMesh(),true);
  cookWorldChunks(base,over);
  auto moved=stripMesh();
  moved.vertices[stripVertex(8,0)].y+=30.f;
  const auto other=writeStripOverride(root/"fallback-edited-other",moved,false);
  const auto before=fs::file_size(over/"chunks/1_0.glb");
  writeBytes(over/"chunks/1_0.glb",padJson(fileBytes(other/"chunks/1_0.glb")));
  require(fs::file_size(over/"chunks/1_0.glb")!=before,"the edited set's new 1_0.glb must have another length");
  expect("an edited set's GLB of another length",compareSet("an edited set's GLB of another length",base,over),0,1,
         "the set's edited ids depend on every cell");
  }

  // A cook stopped after the cells, before the index. The border override moves 1_0's vertex shared with 0_0;
  // its new GLB, of the same length, moves the one shared with 2_0 instead, which renumbers both neighbours'
  // copies at the same totals. The stopped cook leaves no index, so every cell reads its GLB; the completed
  // cook gives every cell cooked, equal to the GLB path.
  {
  const auto base=root/"interrupted", over=root/"interrupted-override";
  writeWorldChunks(stripMesh(),fixtureSource,base);
  writeStripOverride(over,stripMesh(),true);
  cookWorldChunks(base,over);
  auto moved=stripMesh();
  moved.vertices[stripVertex(8,0)].y+=30.f;
  const auto other=writeStripOverride(root/"interrupted-other",moved,false);
  require(fs::file_size(other/"chunks/1_0.glb")==fs::file_size(over/"chunks/1_0.glb"),
          "the edit moved to border 8 must keep its GLB's length");
  fs::copy_file(other/"chunks/1_0.glb",over/"chunks/1_0.glb",fs::copy_options::overwrite_existing);
  const auto cell=fileBytes(over/"chunks/1_0.wcb");
  bool stopped=false;
  try {
    cookWorldChunks(base,over,{},[] { throw std::runtime_error("stopped before the index"); });
    }
  catch(const std::runtime_error& error) {
    stopped=std::string(error.what())=="stopped before the index";
    }
  require(stopped && fileBytes(over/"chunks/1_0.wcb")!=cell,"the cook must stop after writing 1_0's new .wcb");
  require(!fs::exists(over/wci::FileName),"a cook stopped after the cells must leave no index");
  expect("a cook stopped after the cells, before the index",compareSet("a stopped cook",base,over),0,0);
  cookWorldChunks(base,over);
  expectAllCooked("the same cook completed",base,over);
  }

  // 1_0 of the border override with its shift moved from border 4 to border 8: a GLB of the same length.
  const auto movedEdit=[&](const std::string& name) {
    auto moved=stripMesh();
    moved.vertices[stripVertex(8,0)].y+=30.f;
    return writeStripOverride(root/name,moved,false)/"chunks/1_0.glb";
    };

  // Two cooks of one set: A has passed its cells and waits before its index; meanwhile the GLB changes at the
  // same length and B cooks the set completely. A must not publish its older index over B's.
  {
  const auto base=root/"overtaken", over=root/"overtaken-override";
  writeWorldChunks(stripMesh(),fixtureSource,base);
  writeStripOverride(over,stripMesh(),true);
  cookWorldChunks(base,over);
  const auto edited=movedEdit("overtaken-other");
  require(fs::file_size(edited)==fs::file_size(over/"chunks/1_0.glb"),"the moved edit must keep its GLB's length");
  std::string error;
  try {
    cookWorldChunks(base,over,{},[&] {
      fs::copy_file(edited,over/"chunks/1_0.glb",fs::copy_options::overwrite_existing);
      fs::last_write_time(over/"chunks/1_0.glb",
                          std::chrono::floor<std::chrono::seconds>(fs::file_time_type::clock::now()-std::chrono::hours(24)));
      cookWorldChunks(base,over);
      });
    }
  catch(const std::runtime_error& e) {
    error=e.what();
    }
  require(error.find("changed while the world was cooked: its index is not written")!=std::string::npos,
          "a cook overtaken before its index must not write it, got: "+error);
  require(verifyCookedWorld(base,over,{},true).empty(),"the newer cook's cells and index must stay a consistent set");
  expectAllCooked("a cook overtaken by a newer one before its index",base,over);
  }

  // A cook that fails writing its first changed cell: the index went before that write, so none is left.
  {
  const auto base=root/"write-failed", over=root/"write-failed-override";
  writeWorldChunks(stripMesh(),fixtureSource,base);
  writeStripOverride(over,stripMesh(),true);
  cookWorldChunks(base,over);
  fs::copy_file(movedEdit("write-failed-other"),over/"chunks/1_0.glb",fs::copy_options::overwrite_existing);
  fs::remove(over/"chunks/1_0.wcb");
  fs::create_directories(over/"chunks/1_0.wcb/blocker"); // a directory where the cell's file goes: its rename fails
  bool failed=false;
  try {
    cookWorldChunks(base,over);
    }
  catch(const std::runtime_error& e) {
    failed=std::string(e.what()).find("cannot rename")!=std::string::npos;
    }
  require(failed && !fs::exists(over/wci::FileName),"a cook that fails writing a cell must leave no index");
  fs::remove_all(over/"chunks/1_0.wcb");
  cookWorldChunks(base,over);
  expectAllCooked("the same cook after a failed cell write",base,over);
  }

  // The cooked path opens no GLB: with its magic broken at the same length and time, the cooked read still gives
  // the cell, and the GLB read fails.
  {
  dir=copy();
  const auto plain=loadWorld(dir,{},false);
  const auto withCooked=loadWorld(dir,{},true);
  const auto expected=plain.source().read(1,plain.full[1]);
  const auto path=dir/"chunks/1_0.glb";
  const auto time=fs::last_write_time(path);
  auto bytes=fileBytes(path);
  bytes[0]='X';
  writeBytes(path,bytes);
  fs::last_write_time(path,time);
  require(compareReads(expected,withCooked.source().read(1,plain.full[1])).empty() &&
          withCooked.source().reads().glb==0,"the cooked read must not open the GLB");
  bool failed=false;
  try {
    (void)plain.source().read(1,plain.full[1]);
    }
  catch(const std::exception&) {
    failed=true;
    }
  require(failed,"the GLB read of the broken GLB must fail");
  std::puts("A GLB broken at the same length and time: the cooked read gives the cell, the GLB read fails");
  }

  // A .wcb position whose source key is the loaded world's vertex count: outside the world, the cell falls back.
  dir=copy();
  chunk=fileBytes(dir/"chunks/1_0.wcb");
  put32(chunk,get32(chunk,wcb::HeaderSize+16)+12,27); // the source of position 0; the strip has 27 vertices
  writeBytes(dir/"chunks/1_0.wcb",chunk);
  expect("a .wcb source key outside the loaded world",compareSet("a source key outside the world",dir),2,2,
         "position 0 has the stitched id 27, outside the loaded world's 27","chunks/1_0.glb");

  // The GLB changed at the same length and its cell cooked again, the index left from the first cook.
  dir=copy();
  index=fileBytes(dir/wci::FileName);
  auto glb=fileBytes(dir/"chunks/1_0.glb");
  const std::string generator="worldchunks";
  const auto at=std::search(glb.begin(),glb.end(),generator.begin(),generator.end());
  require(at!=glb.end(),"the strip's GLB names its generator");
  *at='W';
  writeBytes(dir/"chunks/1_0.glb",glb);
  require(cookWorldChunks(dir).written==1,"the changed GLB's cell must be cooked again");
  writeBytes(dir/wci::FileName,index);
  expect("a .wcb cooked again without the index",compareSet("a .wcb cooked again without the index",dir),2,2,
         "re-cooked without the index","chunks/1_0.glb");

  // currentFile: the retry of a file changed after world load reads the GLB as it is now, on both sources;
  // a plain read of it fails on both.
  {
  dir=copy();
  const auto plain=loadWorld(dir,{},false);
  const auto withCooked=loadWorld(dir,{},true);
  writeBytes(dir/"chunks/1_0.glb",padJson(fileBytes(dir/"chunks/1_0.glb")));
  for(size_t i=0;i<plain.full.size();++i) {
    const auto difference=compareReads(plain.source().read(i,plain.full[i],true),
                                       withCooked.source().read(i,plain.full[i],true));
    require(difference.empty(),"currentFile: cell "+std::to_string(i)+" differs from the GLB path: "+difference);
    }
  const auto reads=withCooked.source().reads();
  require(reads.cooked==0 && reads.glb==plain.full.size(),"currentFile must take the GLB path for every cell");
  for(const auto* world:{&plain,&withCooked}) {
    bool failed=false;
    try {
      (void)world->source().read(1,world->full[1]);
      }
    catch(const std::runtime_error& error) {
      failed=std::string(error.what()).find("chunk changed after world load")!=std::string::npos;
      }
    require(failed,"a plain read of a GLB changed after world load must fail");
    }
  std::printf("currentFile=true after 1_0.glb changed: %llu of %zu cells from the GLB, equal on both sources; "
              "a plain read fails\n",static_cast<unsigned long long>(reads.glb),plain.full.size());
  }

  // A source ordinal past the cell's triangles fails on the cooked path, naming the file, as on the GLB path.
  {
  dir=copy();
  const auto world=loadWorld(dir,{},true);
  const auto past=std::vector<uint32_t>{uint32_t(world.full[0].size())};
  bool failed=false;
  try {
    (void)world.source().read(0,past);
    }
  catch(const std::runtime_error& error) {
    failed=std::string(error.what()).find("past the "+std::to_string(world.full[0].size())+" triangles of 0_0.wcb")!=
           std::string::npos;
    }
  require(failed,"a source ordinal past the cell's triangles must fail, naming the .wcb");
  require(world.source().reads().cooked==0 && world.source().reads().glb==0,"a failed read counts on no path");
  std::puts("A source ordinal past the cell's triangles: fails, naming 0_0.wcb");
  }
  }

// ---- carried over from task 2b: a cook rewrites a .wcb whose bytes changed at the same length ----

void recookSameLength(const fs::path& root) {
  const auto world=root/"recook", out=root/"recook-cooked";
  writeWorldChunks(stripMesh(),fixtureSource,world);
  cookWorldChunks(world,{},out);
  const auto before=fileBytes(out/"chunks/0_0.wcb");
  // One coordinate of a vertex only cell 0,0 uses.
  auto mesh=stripMesh();
  mesh.vertices[stripVertex(1,0)].y+=1.f;
  writeWorldChunks(mesh,fixtureSource,world);
  const auto stats=cookWorldChunks(world,{},out);
  const auto after=fileBytes(out/"chunks/0_0.wcb");
  require(stats.written==1 && stats.left==2,"the second cook must rewrite only cell 0,0, wrote "+
          std::to_string(stats.written)+" and left "+std::to_string(stats.left));
  require(after.size()==before.size() && after!=before,"cell 0,0's .wcb must keep its length and get the new bytes");
  const auto difference=verifyCookedWorld(world,{},out,true);
  require(difference.empty(),"the re-cooked world must verify with hashes: "+difference);
  std::printf("Second cook after one coordinate changed: 0_0.wcb rewritten at the same %zu bytes, others left; "
              "it verifies with hashes\n",after.size());
  }

// ---- the base's NEWWORLD in a scratch copy ----

size_t cookedFiles(const fs::path& dir) {
  size_t count=0;
  for(const auto& entry:fs::recursive_directory_iterator(dir))
    count+=entry.path().extension()==".wcb" || entry.path().filename()==wci::FileName;
  return count;
  }

// Hard links (or copies across file systems) of every file of a world, without cooked files.
void linkWorld(const fs::path& from, const fs::path& to) {
  fs::create_directories(to);
  for(const auto& entry:fs::recursive_directory_iterator(from)) {
    const auto target=to/fs::relative(entry.path(),from);
    if(entry.is_directory()) {
      fs::create_directories(target);
      continue;
      }
    if(!entry.is_regular_file() || entry.path().extension()==".wcb" || entry.path().filename()==wci::FileName)
      continue;
    std::error_code error;
    fs::create_hard_link(entry.path(),target,error);
    if(error)
      fs::copy_file(entry.path(),target);
    }
  }

// Two threads read the same cells at once, then different ones: each result is the single-thread one.
void concurrentReads(const std::string& label, const Loaded& world, size_t cells) {
  cells=std::min(cells,world.full.size());
  std::vector<WorldChunkMesh> expected;
  for(size_t i=0;i<cells;++i)
    expected.push_back(world.source().read(i,world.full[i]));
  const auto before=world.source().reads().cooked;
  constexpr int rounds=3;
  for(const bool same:{true,false}) {
    std::string errors[2];
    auto run=[&](int thread) {
      try {
        for(int round=0;round<rounds;++round)
          for(size_t i=0;i<cells;++i) {
            if(!same && i%2!=size_t(thread))
              continue;
            const auto difference=compareReads(expected[i],world.source().read(i,world.full[i]));
            if(!difference.empty()) {
              errors[thread]="cell "+std::to_string(i)+": "+difference;
              return;
              }
            }
        }
      catch(const std::exception& error) {
        errors[thread]=error.what();
        }
      };
    std::thread first(run,0), second(run,1);
    first.join();
    second.join();
    for(const auto& error:errors)
      require(error.empty(),label+": two threads reading "+(same ? "the same" : "different")+" cells at once: "+error);
    }
  const auto reads=world.source().reads().cooked-before;
  require(reads==uint64_t(rounds)*(2*cells+cells),label+": every concurrent read must take the cooked path");
  std::printf("%s: two threads reading the same and different cells at once (%llu reads of %zu cells) give the "
              "single-thread results\n",label.c_str(),static_cast<unsigned long long>(reads),cells);
  }

void newWorld(const fs::path& root, const fs::path& world) {
  const auto scratch=root/"NEWWORLD";
  const auto inBase=cookedFiles(world);
  linkWorld(world,scratch);
  const auto started=std::chrono::steady_clock::now();
  const auto stats=cookWorldChunks(scratch);
  std::printf("NEWWORLD cooked beside its GLBs in %s: %zu cells, %llu .wcb bytes, %.1f s\n",scratch.string().c_str(),
              stats.cells,static_cast<unsigned long long>(stats.wcbBytes),
              std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count());
  require(cookedFiles(world)==inBase,"the base must not be written");
  const auto outcome=compareSet("base NEWWORLD",scratch);
  expect("base NEWWORLD",outcome,outcome.cells,1,"cells read from their .wcb");
  std::printf("base NEWWORLD: cooked reads %.1f ms, GLB reads %.1f ms for %zu cells twice\n",outcome.cooked.cookedMs,
              outcome.glb.glbMs,outcome.cells);
  concurrentReads("base NEWWORLD",loadWorld(scratch,{},true),24);
  }

}

int main() {
  TemporaryDirectory temporary;
  try {
    fs::create_directories(temporary.path);
    fixtureSets(temporary.path);
    overrideDirectory(temporary.path);
    fallbacks(temporary.path);
    recookSameLength(temporary.path);

    const auto* gothic=std::getenv("GOTHIC_DIR");
    require(gothic!=nullptr && *gothic!='\0',"GOTHIC_DIR must point to a Gothic II installation");
    const char* args[]={"gothic-wcb-source-contract","-g",gothic};
    CommandLine commandLine(3,args);
    Resources resources;
    Resources::loadBase();
    const auto world=Resources::worldBaseDir("NEWWORLD.ZEN");
    require(!world.empty() && fs::exists(world/"world.json"),"the base has no NEWWORLD");
    newWorld(temporary.path,world);
    std::puts("Cooked chunk source contract passed");
    return EXIT_SUCCESS;
    }
  catch(const std::exception& error) {
    std::fprintf(stderr,"FAIL: %s\n",error.what());
    return EXIT_FAILURE;
    }
  }

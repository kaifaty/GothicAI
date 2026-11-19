// Cooked binary chunks (.wcb) and their index (chunks.wci): cook -> assemble
// equals readWorldChunks for the world base contract's fixtures, edited
// overrides and the base's NEWWORLD; a cell's .wcb is byte for byte the same
// whatever other cells and overrides the set holds (the override question of
// docs/landscape-plan.md, Д3, tasks 2 and 2b); a second cook rewrites nothing
// unchanged, and two cooks into the same files at once both finish; v4
// hierarchies verify byte for byte, their visibility cuts match selectCut, v2
// still reads, and malformed, stale and mismatched files are rejected.
// Needs GOTHIC_DIR and a ready base for NEWWORLD.

#include "commandline.h"
#include "gothicglb.h"
#include "graphics/drawclusters.h"
#include "graphics/sceneglobals.h"
#include "resources.h"
#include "shader.h"
#include "worldchunkcook.h"
#include "worldchunk_fixtures.h"
#include "worldchunk_strip.h"
#include "worldchunks.h"

#include <Tempest/Device>
#include <Tempest/Fence>
#include <Tempest/Pixmap>
#ifdef __APPLE__
#include <Tempest/MetalApi>
#else
#include <Tempest/VulkanApi>
#endif

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
#include <numeric>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {
namespace fs = std::filesystem;
using namespace WorldChunkFixtures;
#ifdef __APPLE__
using GraphicsApi = Tempest::MetalApi;
#else
using GraphicsApi = Tempest::VulkanApi;
#endif

void require(bool condition, const std::string& message) {
  if(!condition)
    throw std::runtime_error(message);
  }

void checkHiZCoverage() {
  struct Case {
    const char* label;
    std::array<float,4> rect;
    int mip;
    std::set<std::pair<int,int>> texels;
    Tempest::Point screen = {89,53};
    Tempest::Point tile = {16,16};
    };
  const Case cases[] = {
    {"upper edge on tile boundary", {15.75f,18.f,16.f,20.f}, 0, {{0,1},{1,1}}},
    {"lower edge on tile boundary", {16.f,18.f,18.f,20.f}, 0, {{1,1}}},
    {"lower edge rounded toward a tile boundary", {std::nextafter(16.f,0.f),32.f,16.75f,32.75f}, 0, {{0,2},{1,2}}},
    {"one pixel", {75.25f,36.25f,75.75f,36.75f}, 0, {{4,2}}},
    {"padded screen edge", {79.75f,51.25f,129.f,65.f}, 0, {{4,3},{5,3}}},
    {"finer mip fits", {32.5f,14.f,86.f,17.f}, 1, {{1,0},{2,0}}},
    {"finer mip crosses three tiles", {16.5f,14.f,64.5f,17.f}, 2, {{0,0},{1,0}}},
    {"rectangular pyramid with a collapsed axis", {-1.f,-1.f,89.f,18.f}, 2, {{0,0},{1,0}}, {89,18}},
    {"last mip of a one-texel pyramid", {0.f,0.f,1.f,1.f}, 0, {{0,0}}, {1,1}, {1,1}},
    };
  for(const auto& t:cases) {
    const auto tileIndex=[](float endpoint, int screen, int tile) {
      return int(std::clamp(endpoint,0.f,float(screen-1)))/tile;
      };
    const int lx=tileIndex(t.rect[0],t.screen.x,t.tile.x), ly=tileIndex(t.rect[1],t.screen.y,t.tile.y);
    const int hx=tileIndex(t.rect[2],t.screen.x,t.tile.x), hy=tileIndex(t.rect[3],t.screen.y,t.tile.y);
    int mip=int(std::bit_width(uint32_t(std::max({1,hx-lx,hy-ly}))))-1;
    if((hx>>mip)-(lx>>mip)>1 || (hy>>mip)-(ly>>mip)>1)
      ++mip;
    std::set<std::pair<int,int>> covered, reference;
    for(int y=ly>>mip;y<=hy>>mip;++y)
      for(int x=lx>>mip;x<=hx>>mip;++x)
        covered.emplace(x,y);
    // Enumerate real framebuffer pixels by their half-open cells. Padded pixels cannot win a fragment.
    for(int y=0;y<t.screen.y;++y)
      for(int x=0;x<t.screen.x;++x)
        if(float(x+1)>t.rect[0] && float(y+1)>t.rect[1] && float(x)<=t.rect[2] && float(y)<=t.rect[3])
          reference.emplace(x/(t.tile.x<<mip),y/(t.tile.y<<mip));
    require(mip==t.mip && covered==reference && covered==t.texels,std::string("HiZ coverage: ")+t.label);
    }
  std::printf("HiZ coverage: %zu pixel-boundary, screen-edge and mip cases\n",std::size(cases));
  }

void checkHistoryHiZContract() {
  // Match the inclusive pixel cells and the finest mip that fits the 8x8 sample budget.
  const auto screen = Tempest::Point(198,119);
  constexpr int tile = 16;
  constexpr int sampleWidth = 8;
  struct Case {
    std::array<float,4> rect;
    int mip;
    size_t samples;
    };
  const Case cases[] = {
    {{15.25f,31.f,15.5f,31.f}, 0, 4},         // both axes cross a tile after the raster pad
    {{16.25f,17.f,128.25f,18.f}, 1, 5},       // nine base texels cross before choosing mip 1
    {{195.f,117.f,250.f,171.f}, 0, 1},        // screen edge inside padding of a rectangular pyramid
    };
  for(const auto& test : cases) {
    const float left = test.rect[0] - 1.f, top = test.rect[1] - 1.f;
    const float right = test.rect[2] + 1.f, bottom = test.rect[3] + 1.f;
    const auto cell = [](float pixel, int size) {
      return int(std::clamp(pixel,0.f,float(size - 1))) / tile;
      };
    int lx = cell(left,screen.x), ly = cell(top,screen.y);
    int rx = cell(right,screen.x), ry = cell(bottom,screen.y);
    int mip = 0;
    while(rx - lx >= sampleWidth || ry - ly >= sampleWidth) {
      lx >>= 1;
      ly >>= 1;
      rx >>= 1;
      ry >>= 1;
      ++mip;
      }

    std::set<std::pair<int,int>> covered, reference;
    for(int y = ly; y <= ry; ++y)
      for(int x = lx; x <= rx; ++x)
        covered.emplace(x,y);
    for(int y = 0; y < screen.y; ++y)
      for(int x = 0; x < screen.x; ++x)
        if(float(x + 1) > left && float(y + 1) > top && float(x) <= right && float(y) <= bottom)
          reference.emplace(x / (tile << mip),y / (tile << mip));
    require(mip == test.mip && covered == reference && covered.size() == test.samples,
            "history HiZ covers all touched cells within the mip sample budget");
    }

  const auto hidden = [](float clipW, float znear, float projectedDepth, float depthRoundoff,
                         const std::array<float,2>& texels) {
    const float nearDepth = projectedDepth - (depthRoundoff + 8.f * std::numeric_limits<float>::epsilon());
    return clipW > znear && nearDepth > 0.f &&
           std::all_of(texels.begin(),texels.end(),[nearDepth](float z) { return nearDepth > z; });
    };
  constexpr float depth = 0.625f;
  constexpr float depth16Step = 1.f / 65535.f;
  require(hidden(12.f,10.f,depth + 2.f * depth16Step,depth16Step,{depth,depth}),
          "history HiZ rejects only after the depth-format guard");
  require(!hidden(12.f,10.f,depth,0.f,{depth,depth}) &&
          !hidden(12.f,10.f,depth + depth16Step / 2.f,depth16Step,{depth,depth}) &&
          !hidden(12.f,10.f,0.9f,0.f,{depth,1.f}) &&
          !hidden(10.f,10.f,0.9f,0.f,{depth,depth}),
          "history HiZ keeps equality, depth quantization, holes and the near plane");

  const std::array history = {std::array{16u,17u},std::array{15u,18u}};
  const auto early = [&](uint32_t slot, uint32_t previousCount, uint32_t generation) {
    return slot < previousCount && history[slot][0] >= 16u && history[slot][1] == generation;
    };
  require(early(0,2,17) && !early(0,2,19) && !early(1,2,18) && !early(2,2,20) && !early(0,0,17),
          "history HiZ accepts a retained slot and ignores recycled, unseeded, new and reset slots");
  std::puts("History HiZ: framebuffer edges, mips, holes, ties, near plane, quantization and recycled slots");
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

void printCooked(const std::string& label, const CookedWorldStats& stats) {
  std::printf("%s: %zu cells, %llu triangles, %llu GLB -> %llu .wcb bytes, index %llu bytes; "
              "cook -> assemble equals readWorldChunks\n",
              label.c_str(),stats.cells,static_cast<unsigned long long>(stats.triangles),
              static_cast<unsigned long long>(stats.glbBytes),static_cast<unsigned long long>(stats.wcbBytes),
              static_cast<unsigned long long>(stats.indexBytes));
  }

CookedWorldStats cookedRoundTrip(const fs::path& dir, const fs::path& over, const fs::path& out, const std::string& label) {
  const auto stats=cookWorldChunks(dir,over,out);
  require(stats.cells==readWorldChunkManifest(dir).cells.size(),label+": one cooked chunk per cell");
  require(stats.written+stats.left==stats.cells && fs::exists(cookedIndexPath(dir,over,out)),
          label+": every cell and the index are on disk");
  const auto difference=verifyCookedWorld(dir,over,out,true);
  require(difference.empty(),label+": cook -> assemble differs from readWorldChunks: "+difference);
  printCooked(label,stats);
  return stats;
  }

std::vector<uint8_t> fileBytes(const fs::path& path) {
  return gothicglb::readFile(path);
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

// The GLB order of each cooked triangle, against the base triangle with the
// same corner source keys (its index is its order); added counts those without one.
void checkOrders(const CookedChunk& chunk, const asset::WorldMesh& base, size_t added, const std::string& label) {
  size_t without=0;
  const auto& vi=base.polygons.vertex_indices;
  for(const auto& t:chunk.triangles()) {
    const std::array<uint32_t,3> ids={chunk.positions()[t.vertex[0]].source,chunk.positions()[t.vertex[1]].source,
                                      chunk.positions()[t.vertex[2]].source};
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
    if(p.source!=q.source)
      out.insert("positions.source");
    }
  for(size_t i=0;i<std::min(a.features().size(),b.features().size());++i) {
    const auto& p=a.features()[i];
    const auto& q=b.features()[i];
    if(bits(p.u)!=bits(q.u) || bits(p.v)!=bits(q.v) || p.light!=q.light || bits(p.nx)!=bits(q.nx) ||
       bits(p.ny)!=bits(q.ny) || bits(p.nz)!=bits(q.nz))
      out.insert("features.value");
    if(p.source!=q.source)
      out.insert("features.source");
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
    if(p.lightmap!=q.lightmap || p.flags!=q.flags || p.sector!=q.sector)
      out.insert("triangles.lightmap/flags/sector");
    }
  for(size_t i=0;i<std::min(a.materials().size(),b.materials().size());++i)
    if(a.materials()[i].source!=b.materials()[i].source || !gothicglb::sameMaterial(a.material(i),b.material(i)))
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
  // The assembler opens no GLB: with every GLB of the set moved away it still gives readWorldChunks' result.
  {
  ChunkTable expectedTable, actualTable;
  const auto expectedMesh=readWorldChunks(dir,border,&expectedTable);
  const auto world=readCookedWorld(dir,border,root/"strip-border-cooked");
  for(const auto* d:{&dir,&border})
    fs::rename(*d/"chunks",*d/"chunks-away");
  const auto actual=assembleCookedWorld(world,&actualTable);
  for(const auto* d:{&dir,&border})
    fs::rename(*d/"chunks-away",*d/"chunks");
  const auto difference=compareWorldMeshes(expectedMesh,actual)+compareChunkTables(expectedTable,actualTable);
  require(difference.empty(),"the cooked world assembled without its GLBs differs from readWorldChunks: "+difference);
  }
  for(const auto* cell:{"0_0","1_0","2_0"})
    checkOrders(CookedChunk::read(plain/"chunks"/(std::string(cell)+".wcb")),base,0,std::string("strip ")+cell);
  checkOrders(CookedChunk::read(root/"strip-edit-cooked/chunks/1_0.wcb"),base,2,"edited 1_0");
  // The material the edit added has no source index; those world.json holds unchanged keep theirs.
  {
  const auto edited1=CookedChunk::read(root/"strip-edit-cooked/chunks/1_0.wcb");
  size_t added=0, kept=0;
  for(size_t i=0;i<edited1.materials().size();++i) {
    const auto source=edited1.materials()[i].source;
    const auto name=edited1.material(i).name;
    if(name=="STRIP_EDIT") {
      require(source==wcb::None,"edited 1_0: the material the edit added must have no source index");
      ++added;
      continue;
      }
    require(source<base.materials.size() && base.materials[source].name==name,
            "edited 1_0: material "+name+" must carry its world.json index");
    ++kept;
    }
  require(added==1 && kept==2,"edited 1_0 must hold the added material and both world.json materials");
  // Its one new vertex and one new feature (the middle of the split triangle) carry None, the rest their source ids.
  size_t newPositions=0, newFeatures=0;
  for(const auto& v:edited1.positions())
    if(v.source==wcb::None)
      ++newPositions;
    else
      require(v.source<base.vertices.size(),"edited 1_0: a source position must carry its _VERTEX");
  for(const auto& f:edited1.features())
    if(f.source==wcb::None)
      ++newFeatures;
    else
      require(f.source<base.features.size(),"edited 1_0: a source feature must carry its _FEATURE");
  require(newPositions==1,"edited 1_0: the new vertex must have the source None, "+std::to_string(newPositions)+" have");
  require(newFeatures==1,"edited 1_0: the new feature must have the source None, "+std::to_string(newFeatures)+" have");
  }

  // Without --out the cooked chunks lie beside the GLB each cell is read from.
  const auto beside=root/"strip-beside";
  fs::copy(dir,beside,fs::copy_options::recursive);
  const auto besideOver=root/"strip-beside-override";
  fs::copy(border,besideOver,fs::copy_options::recursive);
  cookWorldChunks(beside,besideOver);
  require(fs::exists(beside/"chunks/0_0.wcb") && fs::exists(beside/"chunks/2_0.wcb") &&
          fs::exists(besideOver/"chunks/1_0.wcb") && !fs::exists(beside/"chunks/1_0.wcb"),
          "a cell read from the override must have its .wcb beside the override GLB, the others beside theirs");
  require(fs::exists(besideOver/wci::FileName) && !fs::exists(beside/wci::FileName),
          "the index of a world with an override lies in the override directory");
  require(verifyCookedWorld(beside,besideOver,{},true).empty(),"cooked chunks beside their GLBs must assemble");
  // The base on its own: its cells are already on disk, only its own 1_0 and its own index are new.
  const auto alone=cookWorldChunks(beside);
  require(alone.written==1 && alone.left==2 && fs::exists(beside/wci::FileName),
          "cooking the base after its override set writes only the cell the override replaced and the base's index");
  require(verifyCookedWorld(beside,{},{},true).empty() && verifyCookedWorld(beside,besideOver,{},true).empty(),
          "the base and its override set must each assemble from the same base cells and their own index");

  // The override question: do the cooked chunks of the cells the override does
  // not replace differ from those cooked without it? Since task 2b: no, byte for byte.
  struct Answer { std::string override, cell; };
  const std::vector<Answer> answers = {
    {"strip-edit-cooked","0_0"},{"strip-edit-cooked","2_0"},{"strip-border-cooked","0_0"},{"strip-border-cooked","2_0"},
    };
  for(const auto& answer:answers) {
    const auto file="chunks/"+answer.cell+".wcb";
    const auto fields=differingFields(CookedChunk::read(plain/file),CookedChunk::read(root/answer.override/file));
    std::printf("Override question: %s, cell %s not replaced, differs from its cook without the override in: %s\n",
                answer.override.c_str(),answer.cell.c_str(),joined(fields).c_str());
    require(fields.empty() && fileBytes(plain/file)==fileBytes(root/answer.override/file),
            "override question: cell "+answer.cell+" of "+answer.override+" must be byte for byte its cook without "
            "the override, differs in "+joined(fields));
    }

  // What depends on the set lies in the index: the dropped order 15, the appended
  // material and, per cell, the local indices whose stitched id is not their source key.
  struct Expected { std::string out; std::vector<uint32_t> missing; std::vector<std::array<size_t,3>> exceptions; size_t appended; };
  const std::vector<Expected> indexes = {
    {"strip-cooked",{},{{0,0,0},{0,0,0},{0,0,0}},0},
    {"strip-edit-cooked",{15},{{0,0,0},{1,1,1},{0,0,0}},1},
    {"strip-border-cooked",{15},{{1,0,0},{1,1,1},{0,0,0}},1},
    };
  for(const auto& expected:indexes) {
    const auto index=CookedIndex::read(root/expected.out/wci::FileName);
    const std::vector<uint32_t> missing(index.missing().begin(),index.missing().end());
    std::vector<std::array<size_t,3>> exceptions;
    std::string list;
    for(size_t i=0;i<index.cells().size();++i) {
      exceptions.push_back({index.vertexExceptions(i).size(),index.featureExceptions(i).size(),
                            index.materialExceptions(i).size()});
      list+=(i==0 ? "" : ", ")+std::to_string(exceptions.back()[0])+"/"+std::to_string(exceptions.back()[1])+"/"+
            std::to_string(exceptions.back()[2]);
      }
    std::printf("Index of %s: %zu bytes, %zu missing orders, %zu appended materials, vertex/feature/material "
                "exceptions per cell: %s\n",expected.out.c_str(),index.byteSize(),missing.size(),
                index.appendedMaterials(),list.c_str());
    require(missing==expected.missing && exceptions==expected.exceptions && index.appendedMaterials()==expected.appended,
            "the index of "+expected.out+" must hold the missing orders, exceptions and appended materials of its edit");
    }

  // Two overridden cells that each add a vertex and a material: each one's .wcb
  // is the one cooked with only that cell overridden, though the set numbers
  // their new vertices and materials differently.
  const auto second=writeStripOverride(root/"strip-second",base,false,{2,0});
  const auto both=root/"strip-both";
  fs::create_directories(both/"chunks");
  fs::copy_file(edit/"chunks/1_0.glb",both/"chunks/1_0.glb");
  fs::copy_file(second/"chunks/2_0.glb",both/"chunks/2_0.glb");
  cookedRoundTrip(dir,second,root/"strip-second-cooked","strip, override of 2_0 alone");
  cookedRoundTrip(dir,both,root/"strip-both-cooked","strip, overrides of 1_0 and 2_0");
  require(fileBytes(root/"strip-both-cooked/chunks/1_0.wcb")==fileBytes(root/"strip-edit-cooked/chunks/1_0.wcb") &&
          fileBytes(root/"strip-both-cooked/chunks/2_0.wcb")==fileBytes(root/"strip-second-cooked/chunks/2_0.wcb"),
          "with two overridden cells each one's .wcb must be byte for byte its cook with only that cell overridden");
  const auto alone2=CookedIndex::read(root/"strip-second-cooked"/wci::FileName);
  const auto both2=CookedIndex::read(root/"strip-both-cooked"/wci::FileName);
  require(alone2.materialExceptions(2).size()==1 && both2.materialExceptions(2).size()==1 &&
          alone2.materialExceptions(2)[0].id==2 && both2.materialExceptions(2)[0].id==3 &&
          alone2.vertexExceptions(2)[0].id!=both2.vertexExceptions(2)[0].id,
          "the two sets must number 2_0's new vertex and material differently");
  std::printf("Two overridden cells: 1_0 and 2_0 each byte for byte their single-override cook; 2_0's new material "
              "is stitched %u alone and %u with 1_0, its new vertex %u and %u\n",alone2.materialExceptions(2)[0].id,
              both2.materialExceptions(2)[0].id,alone2.vertexExceptions(2)[0].id,both2.vertexExceptions(2)[0].id);

  // The same GLBs cooked as part of another world directory with the same
  // world.json source and materials: here without cell 0,0, the edited 1,0 its own chunk.
  const auto other=root/"strip-other";
  fs::copy(dir,other,fs::copy_options::recursive);
  fs::remove(other/"chunks/0_0.glb");
  fs::copy_file(edit/"chunks/1_0.glb",other/"chunks/1_0.glb",fs::copy_options::overwrite_existing);
  writeText(other/"world.json",withChunks(readText(dir/"world.json"),"\"chunks/1_0.glb\",\"chunks/2_0.glb\""));
  cookedRoundTrip(other,{},root/"strip-other-cooked","strip without cell 0,0 and the edited 1,0 as its own chunk");
  require(fileBytes(root/"strip-other-cooked/chunks/1_0.wcb")==fileBytes(root/"strip-edit-cooked/chunks/1_0.wcb") &&
          fileBytes(root/"strip-other-cooked/chunks/2_0.wcb")==fileBytes(plain/"chunks/2_0.wcb"),
          "a GLB cooked as part of another world directory must give the same .wcb byte for byte");
  std::puts("Independence: cells not replaced, two overridden cells and the same GLBs in another world directory "
            "give the same .wcb byte for byte");

  // A second cook of an unchanged world writes no cell; after an override is
  // added only the replaced cell and the index are written. An old time on every
  // file shows a rewrite whatever the clock's resolution.
  const auto recook=root/"strip-recook";
  auto stats=cookWorldChunks(dir,{},recook);
  require(stats.written==3 && stats.left==0 && stats.indexWritten,"a first cook must write every cell and the index");
  const fs::file_time_type old=std::chrono::floor<std::chrono::seconds>(fs::file_time_type::clock::now()-std::chrono::hours(24));
  const std::vector<fs::path> files={recook/"chunks/0_0.wcb",recook/"chunks/1_0.wcb",recook/"chunks/2_0.wcb",
                                     recook/wci::FileName};
  for(const auto& file:files)
    fs::last_write_time(file,old);
  stats=cookWorldChunks(dir,{},recook);
  require(stats.written==0 && stats.left==3 && !stats.indexWritten,"a second cook of an unchanged world must write nothing");
  for(const auto& file:files)
    require(fs::last_write_time(file)==old,file.string()+" must keep its modification time on a second cook");
  stats=cookWorldChunks(dir,edit,recook);
  require(stats.written==1 && stats.left==2 && stats.indexWritten,"adding an override must write its cell and the index");
  require(fs::last_write_time(files[0])==old && fs::last_write_time(files[2])==old &&
          fs::last_write_time(files[1])!=old && fs::last_write_time(files[3])!=old,
          "adding an override must rewrite only the replaced cell and the index");
  require(verifyCookedWorld(dir,edit,recook,true).empty(),"the re-cooked world with its override must assemble");
  std::puts("Second cook: unchanged world 0 of 3 cells written, times kept; with an override 1 cell and the index written");
  }

// Two cooks of the same world into the same folder at once: each writes
// through its own temporary names, both finish, and the result verifies.
CookedWorldStats concurrentCooks(const fs::path& world, const fs::path& out, const std::string& label) {
  std::string errors[2];
  CookedWorldStats stats[2];
  auto cook=[&](int i) {
    try {
      stats[i]=cookWorldChunks(world,{},out);
      }
    catch(const std::exception& error) {
      errors[i]=error.what();
      }
    };
  std::thread first(cook,0), second(cook,1);
  first.join();
  second.join();
  for(int i=0;i<2;++i)
    require(errors[i].empty(),label+": one of two cooks at once failed: "+errors[i]);
  const auto difference=verifyCookedWorld(world,{},out,true);
  require(difference.empty(),label+": the world cooked twice at once differs from readWorldChunks: "+difference);
  std::printf("%s: two cooks into one folder at once both finished (%zu and %zu cells written, %zu and %zu left); "
              "it verifies\n",label.c_str(),stats[0].written,stats[1].written,stats[0].left,stats[1].left);
  return stats[0];
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

uint64_t get64(const std::vector<uint8_t>& bytes, size_t at) {
  return uint64_t(get32(bytes,at)) | (uint64_t(get32(bytes,at+4))<<32);
  }

// CPU mirror of the visibility shader. It walks the same cluster order and
// uses both transition records before any frustum or occlusion test.
std::vector<uint32_t> visibilityCut(wcb::HierarchyView hierarchy, const ClusterLod::View& view, float thresholdPx) {
  constexpr float infinity=std::numeric_limits<float>::infinity();
  const float projectionScale=0.5f*view.verticalProjection*view.viewportHeight;
  const auto errorPx=[&](const ClusterLod::Group& transition) {
    const auto& center=transition.bounds.center;
    const float x=center[0]-view.camera[0], y=center[1]-view.camera[1], z=center[2]-view.camera[2];
    if(view.farDepth>0.f) {
      const float depth=x*view.forward[0]+y*view.forward[1]+z*view.forward[2];
      const float radius=transition.bounds.radius;
      if(depth+radius>=view.farDepth-view.farGuard && depth-radius<=view.farDepth+view.farGuard)
        return infinity;
      }
    const float nearDistance=std::max(std::sqrt(x*x+y*y+z*z)-transition.bounds.radius,1.f);
    return transition.errorCm/nearDistance*projectionScale;
    };
  std::vector<uint32_t> selected;
  for(uint32_t i=0;i<hierarchy.clusters.size();++i) {
    const auto& cluster=hierarchy.clusters[i];
    const auto& parent=hierarchy.groups[cluster.parentGroup];
    const bool leaf=cluster.level==0;
    const bool terminal=parent.errorCm==infinity;
    if(thresholdPx<=0.f ? leaf : thresholdPx==infinity ? terminal :
       (leaf || errorPx(hierarchy.groups[cluster.refinedGroup])<=thresholdPx) &&
       (terminal || errorPx(parent)>thresholdPx))
      selected.push_back(i);
    }
  return selected;
  }

// Drive the same three visibility pipelines as a deterministic game frame
// without a window. A permissive frustum and an empty HiZ isolate the cut.
class GpuVisibilityCut {
  public:
    struct Instance {
      std::array<ClusterLod::Point,4> matrix; // four columns of the shader's affine matrix
      float fatness=0;
      uint32_t animPtr=0, deformPtr=0, deformCount=0;
      };
    static_assert(sizeof(Instance)==64);

    GpuVisibilityCut(Tempest::Device& device, std::span<const ClusterLod::Cluster> meshlets,
                     std::span<const ClusterLod::Group> groups, std::span<const Instance> instances = {})
      : device(device), meshletCount(uint32_t(meshlets.size())), instanced(!instances.empty()) {
      constexpr const char* names[] = {"visibility_pass_ordered.comp.sprv", "visibility_pass_hiz_ordered.comp.sprv",
                                       "visibility_pass_hiz_cr_ordered.comp.sprv"};
      for(size_t i=0;i<pipelines.size();++i) {
        const auto code=GothicShader::get(names[i]);
        pipelines[i]=device.pipeline(device.shader(code.data,code.len));
        require(pipelines[i].sizeofBuffer(5, 2)==2*sizeof(DrawClusters::Cluster),"visibility shader cluster stride");
        require(pipelines[i].sizeofBuffer(9, 1)==sizeof(PackedMesh::ClusterLevel),"visibility shader level record stride");
        }

      std::vector<DrawClusters::Cluster> clusters(meshlets.size()*std::max(size_t(1),instances.size()));
      std::vector<uint32_t> order(clusters.size()), levelIndex(clusters.size());
      std::vector<PackedMesh::ClusterLevel> levels;
      levels.reserve(meshlets.size());
      std::iota(order.begin(),order.end(),0u);
      const auto sphere=[](const ClusterLod::Sphere& s) {
        return Tempest::Vec4{s.center[0],s.center[1],s.center[2],s.radius};
        };
      for(const auto& cluster:meshlets) {
        const auto& parent=groups[cluster.parentGroup];
        const auto own=cluster.level==0 ? ClusterLod::Group{} : groups[cluster.refinedGroup];
        levels.push_back({sphere(own.bounds),sphere(parent.bounds),own.errorCm,parent.errorCm,cluster.level,1});
        }
      for(size_t i=0;i<clusters.size();++i) {
        clusters[i].r=1.f;
        clusters[i].firstMeshlet=uint32_t(i%meshlets.size());
        clusters[i].meshletCount=1;
        clusters[i].instanceId=instanceId(uint32_t(i));
        levelIndex[i]=clusters[i].firstMeshlet+1u;
        }
      if(instanced) {
        // A discrete tree in the same dispatch keeps its own shadow range and command.
        order.push_back(uint32_t(clusters.size()));
        levelIndex.push_back(0);
        auto& tree=clusters.emplace_back();
        tree.r=1.f;
        tree.firstMeshlet=meshletCount;
        tree.meshletCount=1;
        tree.shadowFirstMeshlet=meshletCount+1;
        tree.shadowMeshletCount=1;
        tree.shadowCommandId=1;
        }

      // A zero view-project matrix makes the instance's box take the near-plane fallback.
      // The 8x8 HiZ makes its full-screen box large enough for the occluder pipeline.
      SceneGlobals::UboGlobal globals{};
      const uint32_t blank[36]{};
      globals.viewFlags=SceneGlobals::VF_HIZ_PRECISE;
      scene=device.ssbo(&globals,sizeof(globals));
      globals.viewFlags=SceneGlobals::VF_SHADOW;
      shadowScene=device.ssbo(&globals,sizeof(globals));
      dummy=device.ssbo(blank,sizeof(blank));
      if(instanced)
        instanceBuf=device.ssbo(instances.data(),instances.size_bytes());
      hiZ=device.texture(Tempest::Pixmap(8,8,Tempest::TextureFormat::RGBA8),false);
      indirect=device.ssbo(Tempest::Uninitialized,2*sizeof(Indirect));
      clusterBuf=device.ssbo(clusters);
      orderBuf=device.ssbo(order);
      levelIndices=device.ssbo(levelIndex);
      records=device.ssbo(levels);
      payload=device.ssbo(Tempest::Uninitialized,clusters.size()*sizeof(Payload));
      count=uint32_t(clusters.size());
      }

    void compare(const ClusterLod::View& view, float thresholdPx, const std::vector<uint32_t>& expected) {
      struct Push {
        uint32_t firstMeshlet, meshletCount;
        float znear;
        uint32_t pad;
        float lodPx, lodProjection, lodHeight;
        uint32_t lodDebug;
        float lodCamera[4];
        float lodForwardAndGuard[4];
        };
      static_assert(sizeof(Push)==64);
      const Push push={0,count,1.f,0,thresholdPx,view.verticalProjection,view.viewportHeight,0,
                       {view.camera[0],view.camera[1],view.camera[2],view.farDepth},
                       {view.forward[0],view.forward[1],view.forward[2],view.farGuard}};
      for(size_t pass=0;pass<pipelines.size();++pass) {
        const bool shadow=pass==0;
        const bool separateShadow=instanced && shadow;
        Indirect draws[2]{};
        draws[1].writeOffset=uint32_t(expected.size());
        indirect.update(draws,0,sizeof(draws));
        auto command=device.commandBuffer();
        {
          auto encoder=command.startEncoding(device);
          encoder.setBinding(0,shadow ? shadowScene : scene);
          encoder.setBinding(1,payload);
          encoder.setBinding(2,instanced ? instanceBuf : dummy);
          encoder.setBinding(3,dummy);
          encoder.setBinding(4,indirect);
          encoder.setBinding(5,clusterBuf);
          encoder.setBinding(6,orderBuf);
          encoder.setBinding(7,hiZ);
          encoder.setBinding(8,levelIndices);
          encoder.setBinding(9,records);
          if(pass==1)
            encoder.setBinding(10,dummy);
          encoder.setPipeline(pipelines[pass]);
          encoder.setPushData(push);
          encoder.dispatchThreads(1);
        }
        device.submit(command).wait();
        device.readBytes(indirect,draws,sizeof(draws));
        constexpr const char* names[] = {"shadow", "main", "HiZ"};
        const auto label=std::string("GPU ")+names[pass]+(instanced ? " instanced" : "")+
                         " visibility at "+std::to_string(thresholdPx)+" px";
        const size_t visualCount=expected.size()+(instanced && !shadow ? 1u : 0u);
        require(draws[0].instanceCount==visualCount && draws[1].instanceCount==(separateShadow ? 1u : 0u),
                label+": wrong meshlet count in the visual or separate shadow command");
        std::vector<Payload> visible(expected.size()+(instanced ? 1u : 0u));
        if(visible.empty())
          continue;
        device.readBytes(payload,visible.data(),visible.size()*sizeof(Payload));
        require(std::ranges::equal(std::span(visible).first(expected.size()),expected,[&](const Payload& value, uint32_t index) {
          return value.instance==instanceId(index) && value.meshlet==index%meshletCount;
          }),label+": instance or cluster IDs differ from selectCut");
        if(instanced)
          require(visible.back().instance==0 && visible.back().meshlet==meshletCount+(separateShadow ? 1u : 0u),
                  label+": the tree did not use the meshlet range for its view");
        }
      }

  private:
    uint32_t instanceId(uint32_t index) const { return instanced ? index/meshletCount : 0xFFFFFFFFu; }

    struct Indirect {
      uint32_t vertexCount,instanceCount,firstVertex,firstInstance,writeOffset;
      };
    struct Payload {
      uint32_t instance,meshlet,bucket,flags;
      };
    Tempest::Device& device;
    std::array<Tempest::ComputePipeline,3> pipelines;
    Tempest::StorageBuffer scene,shadowScene,dummy,instanceBuf,indirect,clusterBuf,orderBuf,levelIndices,records,payload;
    Tempest::Texture2d hiZ;
    uint32_t meshletCount=0;
    bool instanced=false;
    uint32_t count=0;
  };

void checkGpuHistoryDepth(Tempest::Device& device) {
  using namespace Tempest;
  constexpr uint32_t side = 64;
  constexpr uint32_t owner = 23;
  const auto shader = [&](const char* name) {
    const auto code = GothicShader::get(name);
    return device.shader(code.data,code.len);
    };

  RenderState state;
  state.setCullFaceMode(RenderState::CullMode::NoCull);
  state.setZTestMode(RenderState::ZTestMode::LEqual);
  const auto geometry = shader("main_obj_g_orm_hist_slot.vert.sprv");
  const auto depthOnly = device.pipeline(Topology::Triangles,state,geometry,shader("main_obj_g_at_orm_hist_depth_slot.frag.sprv"));
  const auto depthProbe = device.pipeline(Topology::Triangles,state,geometry,shader("main_obj_g_at_orm_hist_depth_verify_slot.frag.sprv"));
  const auto gbuffer = device.pipeline(Topology::Triangles,state,geometry,shader("main_obj_g_at_orm_hist_slot.frag.sprv"));
  const auto baseline = device.pipeline(Topology::Triangles,state,shader("main_obj_g_orm_slot.vert.sprv"),
                                         shader("main_obj_g_at_orm_slot.frag.sprv"));
  const auto copyDepth = device.pipeline(shader("ssao_depth.comp.sprv"));

  // A perspective quad with hashed alpha: test holes, interpolated depths and
  // the exact same packed payload layout the two tree history passes consume.
  constexpr Resources::Vertex vertices[] = {
    {{-0.85f,-0.85f,0.29f},{0.f,0.f,1.f},{0.f,0.f},0xffffffffu},
    {{ 0.85f,-0.85f,0.54f},{0.f,0.f,1.f},{1.f,0.f},0xffffffffu},
    {{ 0.85f, 0.85f,0.65f},{0.f,0.f,1.f},{1.f,1.f},0xffffffffu},
    {{-0.85f, 0.85f,0.40f},{0.f,0.f,1.f},{0.f,1.f},0xffffffffu},
    };
  uint32_t indices[PackedMesh::MaxPrim] = {};
  indices[0] = 1u << 8u | 2u << 16u;
  indices[1] = 2u << 8u | 3u << 16u;
  indices[PackedMesh::MaxPrim - 1] = 2u << 16u | 4u << 24u;
  const GpuVisibilityCut::Instance instance = {{{{1.f,0.03f,0.f},{0.f,1.f,0.f},{0.f,0.f,1.f},{0.02f,0.f,0.f}}}};
  const uint32_t task[] = {0u,0u,0u,owner};
  uint32_t bucket[36] = {};
  bucket[12] = std::bit_cast<uint32_t>(1.f); // alphaWeight
  bucket[14] = 0x200u; // BK_HASHED_ALPHA
  bucket[15] = std::bit_cast<uint32_t>(1.f); // uvScale
  SceneGlobals::UboGlobal globals{};
  globals.viewProject = Matrix4x4::mkIdentity();
  globals.viewProject[2][3] = 0.17f;
  globals.viewProject[3][0] = 0.005f;
  globals.viewFlags = SceneGlobals::VF_TAA;
  globals.taaFrame = 7u;
  globals.screenRes = {int(side),int(side)};
  const auto scene = device.ssbo(&globals,sizeof(globals));
  const auto instances = device.ssbo(&instance,sizeof(instance));
  const auto payload = device.ssbo(task,sizeof(task));
  const auto buckets = device.ssbo(bucket,sizeof(bucket));
  const auto ibo = device.ssbo(indices,sizeof(indices));
  const auto vbo = device.ssbo(vertices,sizeof(vertices));

  Pixmap checker(4,4,TextureFormat::RGBA8);
  auto* pixels = static_cast<uint8_t*>(checker.data());
  for(uint32_t y = 0; y < 4; ++y) {
    for(uint32_t x = 0; x < 4; ++x) {
      const uint32_t at = (y * 4 + x) * 4;
      pixels[at + 0] = pixels[at + 1] = pixels[at + 2] = 255u;
      pixels[at + 3] = ((x + y) % 2u) != 0u ? 73u : 255u;
      }
    }
  const auto texture = device.texture(checker,false);
  auto depth = device.zbuffer(TextureFormat::Depth32F,side,side);
  auto diffuse = device.attachment(TextureFormat::RGBA8,side,side);
  auto normal = device.attachment(TextureFormat::R32U,side,side);
  auto orm = device.attachment(TextureFormat::RGBA8,side,side);
  auto motion = device.attachment(TextureFormat::RG16F,side,side);
  auto earlyOwner = device.attachment(TextureFormat::R32U,side,side);
  auto finalOwner = device.attachment(TextureFormat::R32U,side,side);
  std::array<StorageImage,4> depths;
  for(auto& copy : depths)
    copy = device.image2d(TextureFormat::R32F,side,side);

  auto command = device.commandBuffer();
  {
    auto encoder = command.startEncoding(device);
    const auto sampler = Sampler::nearest(ClampMode::ClampToEdge);
    const uint32_t push[] = {0u,1u};
    const auto draw = [&](const RenderPipeline& pipeline, StorageImage& copy) {
      encoder.setBinding(0,scene);
      encoder.setBinding(1,payload);
      encoder.setBinding(2,instances);
      encoder.setBinding(3,buckets);
      encoder.setBinding(4,ibo);
      encoder.setBinding(5,vbo);
      encoder.setBinding(6,texture);
      encoder.setBinding(7,sampler);
      encoder.setBinding(15,texture);
      encoder.setBinding(16,texture);
      encoder.setBinding(21,instances);
      encoder.setPushData(push);
      encoder.setPipeline(pipeline);
      encoder.draw(nullptr,0,PackedMesh::MaxInd);
      encoder.setFramebuffer({});
      encoder.setBinding(0,depth,sampler);
      encoder.setBinding(1,copy);
      encoder.setPipeline(copyDepth);
      encoder.dispatchThreads(side,side);
      };

    encoder.setFramebuffer({{diffuse,Vec4(),Preserve},{normal,Vec4(),Preserve},{orm,Vec4(),Preserve},
                            {motion,Vec4(),Preserve}}, {depth,1.f,Preserve});
    draw(baseline,depths[3]);
    encoder.setFramebuffer({}, {depth,1.f,Preserve});
    draw(depthOnly,depths[0]);
    encoder.setFramebuffer({{earlyOwner,Vec4(),Preserve}}, {depth,1.f,Preserve});
    draw(depthProbe,depths[1]);
    encoder.setFramebuffer({{diffuse,Vec4(),Preserve},{normal,Vec4(),Preserve},{orm,Vec4(),Preserve},
                            {motion,Vec4(),Preserve},{finalOwner,Vec4(),Preserve}},
                           {depth,Preserve,Preserve});
    draw(gbuffer,depths[2]);
    }
  device.submit(command).wait();

  const auto equal = [](const Pixmap& a, const Pixmap& b) {
    return a.dataSize() == b.dataSize() && std::memcmp(a.data(),b.data(),a.dataSize()) == 0;
    };
  const auto finalDepth = device.readPixels(depths[2]);
  require(equal(finalDepth,device.readPixels(depths[0])) && equal(finalDepth,device.readPixels(depths[1])) &&
          equal(finalDepth,device.readPixels(depths[3])),
          "GPU history depth, GBuffer and 3a disagree on cutout coverage or depth values");
  const auto owners = device.readPixels(finalOwner);
  require(equal(owners,device.readPixels(earlyOwner)),
          "GPU history depth and GBuffer disagree on cutout owners");

  size_t interiorWinners = 0;
  for(uint32_t y = 16; y < 48; ++y) {
    for(uint32_t x = 16; x < 48; ++x) {
      uint32_t found = 0;
      const auto* data = static_cast<const uint8_t*>(owners.data());
      std::memcpy(&found,data + (y * side + x) * sizeof(found),sizeof(found));
      require(found == 0u || found == owner,"GPU history writes the packed cluster owner");
      interiorWinners += size_t(found == owner);
      }
    }
  require(interiorWinners > 0 && interiorWinners < 32u * 32u,"GPU history fixture has covered pixels and alpha holes");
  std::printf("GPU history depth: bitwise coverage and owner agreement, %zu covered interior pixels\n",interiorWinners);
  }

size_t checkVisibilityCuts(const CookedChunk& chunk, const fs::path& path, Tempest::Device* device=nullptr) {
  const auto hierarchy=chunk.hierarchy();
  ClusterLod::Graph graph;
  graph.clusters.assign(hierarchy.clusters.begin(),hierarchy.clusters.end());
  graph.groups.assign(hierarchy.groups.begin(),hierarchy.groups.end());
  const auto& lo=chunk.header().min;
  const auto& hi=chunk.header().max;
  const float x=(lo.x+hi.x)*0.5f, y=(lo.y+hi.y)*0.5f, z=(lo.z+hi.z)*0.5f;
  const ClusterLod::View views[] = {
    {{x,y,z},1440.f,1.496606f}, {{x+3200.f,y+800.f,z-6400.f},1080.f,1.496606f},
    {{x-16400.f,y+7500.f,z+32000.f},1440.f,2.414214f}, {{x+200000.f,y+30000.f,z+600000.f},720.f,1.191754f},
    };
  constexpr float infinity=std::numeric_limits<float>::infinity();
  size_t samples=0;
  std::optional<GpuVisibilityCut> gpu;
  if(device!=nullptr)
    gpu.emplace(*device,hierarchy.clusters,hierarchy.groups);
  const auto compare=[&](const ClusterLod::View& view, float thresholdPx) {
    const auto actual=visibilityCut(hierarchy,view,thresholdPx);
    const auto expected=ClusterLod::selectCut(graph,view,thresholdPx);
    require(actual==expected,path.string()+": visibility cut differs from selectCut in sample "+
            std::to_string(samples)+" at "+std::to_string(thresholdPx)+" px");
    if(gpu)
      gpu->compare(view,thresholdPx,expected);
    ++samples;
    };
  for(const auto& view:views)
    for(float threshold:{0.f,1.f,8.f,infinity})
      compare(view,threshold);

  // On a finite transition, equality must keep the replacement and exclude
  // the clusters it replaces. Check the immediately adjacent thresholds too.
  const auto transition=std::ranges::find_if(graph.groups,[](const ClusterLod::Group& group) {
    return group.errorCm>0.f && std::isfinite(group.errorCm);
    });
  if(transition!=graph.groups.end()) {
    const ClusterLod::View view={transition->bounds.center,1440.f,1.496606f};
    const float threshold=ClusterLod::projectError(*transition,view);
    for(float nearby:{std::nextafter(threshold,0.f),threshold,std::nextafter(threshold,infinity)})
      compare(view,nearby);

    auto distant=view;
    distant.camera[2]-=100000.f;
    const float distantThreshold=ClusterLod::projectError(*transition,distant)*1.125f;
    compare(distant,distantThreshold);
    const auto normalCut=ClusterLod::selectCut(graph,distant,distantThreshold);
    distant.forward={0.f,0.f,1.f};
    distant.farDepth=100000.f;
    distant.farGuard=1000.f;
    require(ClusterLod::selectCut(graph,distant,distantThreshold)!=normalCut,
            path.string()+": the far clip guard did not refine the transition at 1 km");
    compare(distant,distantThreshold);
    distant.farDepth+=transition->bounds.radius+distant.farGuard+1.f;
    compare(distant,distantThreshold);
    }
  return samples;
  }

void checkInstancedVisibilityCuts(Tempest::Device& device) {
  constexpr float infinity=std::numeric_limits<float>::infinity();
  const ClusterLod::Sphere bounds={{400.f,80.f,300.f},30.f};
  ClusterLod::Graph graph;
  graph.groups={{bounds,2.f},{bounds,infinity}};
  graph.clusters={{bounds,0,0,ClusterLod::NoGroup,0,0,0,3,1},
                  {bounds,0,0,ClusterLod::NoGroup,0,0,0,3,1},
                  {bounds,1,0,0,1,0,0,3,1}};
  const GpuVisibilityCut::Instance instances[]={
    {{{{0.f,0.f,-3.f},{0.f,2.f,0.f},{1.f,0.f,0.f},{800.f,50.f,4000.f}}}},
    {{{{-1.5f,0.f,0.f},{0.f,1.f,0.f},{0.f,0.f,-0.75f},{-300.f,180.f,9000.f}}}},
    };
  // These matrices are a rotation times a diagonal scale. The largest scale
  // is known for each, so the CPU reference need not mirror the shader's bound.
  const float scales[]={3.f,1.5f};
  std::array<ClusterLod::Graph,std::size(instances)> worldGraphs={graph,graph};
  for(size_t i=0;i<worldGraphs.size();++i) {
    auto matrix=Tempest::Matrix4x4::mkIdentity();
    for(size_t c=0;c<4;++c)
      for(size_t r=0;r<3;++r)
        matrix[c][r]=instances[i].matrix[c][r];
    for(auto& group:worldGraphs[i].groups) {
      auto& center=group.bounds.center;
      matrix.project(center[0],center[1],center[2]);
      group.bounds.radius*=scales[i];
      group.errorCm*=scales[i];
      }
    }

  GpuVisibilityCut gpu(device,graph.clusters,graph.groups,instances);
  const auto expectedCut=[&](const ClusterLod::View& view, float thresholdPx) {
    std::vector<uint32_t> selected;
    for(size_t i=0;i<worldGraphs.size();++i) {
      const uint32_t first=uint32_t(i*graph.clusters.size());
      for(auto cluster:ClusterLod::selectCut(worldGraphs[i],view,thresholdPx))
        selected.push_back(first+cluster);
      }
    return selected;
    };
  ClusterLod::View view={{},1440.f,1.496606f};
  for(float threshold:{0.f,1.f,1.75f,8.f,infinity})
    gpu.compare(view,threshold,expectedCut(view,threshold));
  require(expectedCut(view,1.75f)==std::vector<uint32_t>{0,1,5},
          "the two instances must select different levels at the same threshold");

  view.forward={0.f,0.f,1.f};
  view.farGuard=2.f;
  const auto& second=worldGraphs[1].groups.front().bounds;
  view.farDepth=second.center[2]+second.radius+view.farGuard-1.f;
  require(expectedCut(view,1.f)==std::vector<uint32_t>{0,1,3,4},
          "the far clip guard must refine the second instance in world space");
  gpu.compare(view,1.f,expectedCut(view,1.f));
  view.farDepth+=2.f;
  gpu.compare(view,1.f,expectedCut(view,1.f));
  std::puts("GPU instanced visibility cut: 7 comparisons per pipeline (main, HiZ and shadow) on two instances and a tree with a separate shadow range and command");
  }

void hierarchyRoundTrip(const fs::path& root) {
  const auto base=root/"hierarchy";
  const auto dir=base/"worlds"/"CONTRACT";
  const auto textures=base/"textures";
  fs::create_directories(textures);
  writeText(textures/"textures.json",R"({"textures":{"OPAQUE":"opaque.dds","MASKED":"masked.dds"}})");
  for(const auto& [name,fourcc,size]:{std::tuple{"opaque.dds","DXT1",136u},std::tuple{"masked.dds","DXT3",144u}}) {
    std::vector<uint8_t> dds(size);
    std::copy_n("DDS ",4,dds.begin());
    put32(dds,4,124);
    put32(dds,12,4);
    put32(dds,16,4);
    put32(dds,76,32);
    std::copy_n(fourcc,4,dds.begin()+84);
    gothicglb::writeFile(textures/name,dds.data(),dds.size());
    }
  writeWorldChunks(selectionMesh(),fixtureSource,dir);
  std::vector<asset::Material> materials(4);
  for(auto& material:materials)
    material.color.a=255;
  materials[0].texture="opaque.tga";
  materials[1].texture="masked.tga";
  materials[2].texture="missing.tga";
  materials[3].texture="opaque.tga";
  materials[3].dont_collapse=true;
  require(clusterLodLockedMaterials(dir,materials)==std::vector<uint8_t>{0,1,1,1},
          "the texture index must distinguish opaque, alpha-tested, missing and explicitly locked materials");
  cookWorldChunks(dir);
  const auto path=dir/"chunks/0_0.wcb";
  const auto bytes=fileBytes(path);
  require(get32(bytes,4)==wcb::Version && get32(bytes,124)==10,"the cook must write v4 with ten sections");
  const auto chunk=CookedChunk::open(bytes,path.string());
  const auto hierarchy=chunk.hierarchy();
  require(hierarchy.vertices.size()==3 && hierarchy.clusters.size()==1 && hierarchy.groups.size()==1 &&
          hierarchy.vertexIndices.size()==3 && hierarchy.indices.size()==3,"a single triangle's cooked hierarchy");
  require(verifyCookedWorld(dir,{},{},true).empty(),"the v4 hierarchy must match a fresh build");
  checkVisibilityCuts(chunk,path);

  constexpr size_t baseSections=5, hierarchyDirectory=baseSections*wcb::EntrySize;
  constexpr size_t strings=wcb::HeaderSize+4*wcb::EntrySize, clusters=wcb::HeaderSize+5*wcb::EntrySize,
                   indices=wcb::HeaderSize+7*wcb::EntrySize;
  auto changed=bytes;
  const auto firstIndex=size_t(get64(bytes,indices+16));
  // A cyclic permutation leaves the same triangle and is readable, but must fail a byte comparison.
  changed[firstIndex]   = bytes[firstIndex+1];
  changed[firstIndex+1] = bytes[firstIndex+2];
  changed[firstIndex+2] = bytes[firstIndex];
  gothicglb::writeFile(path,changed.data(),changed.size());
  (void)CookedChunk::read(path,true);
  require(verifyCookedWorld(dir,{},{},true).find("cluster hierarchy indices")!=std::string::npos,
          "verification must report which hierarchy section changed");

  const auto readHierarchy=[&] { (void)CookedChunk::read(path,true); };
  changed=bytes;
  changed[firstIndex]=uint8_t(hierarchy.clusters.front().vertexCount);
  gothicglb::writeFile(path,changed.data(),changed.size());
  expectThrows(readHierarchy,{"0_0.wcb","local indices","meshlet-local vertex index out of range"},
               "a local index past the cluster's vertices");
  changed=bytes;
  const auto firstCluster=size_t(get64(bytes,clusters+16));
  put32(changed,firstCluster+offsetof(ClusterLod::Cluster,triangleCount),2);
  gothicglb::writeFile(path,changed.data(),changed.size());
  expectThrows(readHierarchy,{"0_0.wcb","local indices","invalid cluster triangle range"},
               "a cluster triangle range past the local index section");

  // The v3 layout is still usable for level 0, even if its graph is unusable.
  put32(changed,4,wcb::OldHierarchyVersion);
  gothicglb::writeFile(path,changed.data(),changed.size());
  const auto previous=CookedChunk::read(path,true);
  require(previous.version()==wcb::OldHierarchyVersion && previous.hierarchy().byteSize()==0 &&
          previous.byteSize()<changed.size() && differingFields(chunk,previous).empty(),
          "v3 must read only the original positions, features, triangles and materials");

  auto legacy=bytes;
  legacy.resize(size_t(get64(bytes,strings+16)+get64(bytes,strings+24)));
  const auto newSections=legacy.begin()+wcb::HeaderSize+baseSections*wcb::EntrySize;
  legacy.erase(newSections,newSections+hierarchyDirectory);
  for(size_t i=0;i<baseSections;++i) {
    const size_t at=wcb::HeaderSize+i*wcb::EntrySize+16;
    put64(legacy,at,get64(legacy,at)-hierarchyDirectory);
    }
  put32(legacy,4,2);
  put32(legacy,124,baseSections);
  put64(legacy,16,legacy.size());
  gothicglb::writeFile(path,legacy.data(),legacy.size());
  for(const bool wantHierarchy:{true,false}) {
    const auto old=CookedChunk::read(path,wantHierarchy);
    const auto oldHierarchy=old.hierarchy();
    require(old.version()==2 && oldHierarchy.vertices.empty() && oldHierarchy.clusters.empty() &&
            oldHierarchy.groups.empty() && oldHierarchy.vertexIndices.empty() && oldHierarchy.indices.empty(),
            "the file reader must accept v2 without a hierarchy");
    require(differingFields(chunk,old).empty(),"v2 keeps the same positions, features, triangles and materials");
    }
  std::puts("Cluster hierarchy: v4 round trip and byte verification, bad indices rejected, v2 and v3 read as geometry");
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
  put32(bytes,4,1);
  expectThrows(open(bytes),{"header","version 1"},"a version 1 .wcb, with rank and resolved ids");
  bytes=good;
  put64(bytes,triangles+16,(good.size()+31)/16*16);
  expectThrows(open(bytes),{"triangles","past the end of the file"},"a section past the end of the file");
  bytes=good;
  const auto length=good.size()-get64(good,strings+16)+1;
  put32(bytes,strings+8,uint32_t(length));
  put64(bytes,strings+24,length);
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
  std::puts("Rejections: truncated, short, wrong magic, version, version 1, a section starting and one ending past "
            "the end, two count mismatches, GLB of another length and of another SHA-256; SHA-256 vector");
  }

// The index: malformed, of another world, with other cells than world.json, an
// entry that names another GLB than its cell's .wcb, exceptions out of range, missing.
void indexRejections(const fs::path& root) {
  const auto dir=root/"stale"; // cooked by rejections(), GLBs restored
  const auto indexFile=dir/wci::FileName;
  const auto good=fileBytes(indexFile);
  const auto open=[](const std::vector<uint8_t>& bytes) {
    return [bytes] { (void)CookedIndex::open(bytes,"fixture.wci"); };
    };
  (void)CookedIndex::open(good,"fixture.wci");
  auto bytes=good;
  bytes.pop_back();
  expectThrows(open(bytes),{"chunk index fixture.wci","header","truncated"},"a truncated index");
  bytes.assign(good.begin(),good.begin()+50);
  expectThrows(open(bytes),{"header","truncated: 50 bytes"},"an index shorter than its header");
  bytes=good;
  bytes[0]='X';
  expectThrows(open(bytes),{"header","wrong magic"},"an index with a wrong magic");
  bytes=good;
  put32(bytes,4,wci::Version+1);
  expectThrows(open(bytes),{"header","version "+std::to_string(wci::Version+1)},"an index of another version");

  // Against world.json and the cells, through readCookedWorld; each case writes the index in place.
  const auto readWith=[&](const fs::path& world, const fs::path& over, const fs::path& out,
                          const fs::path& file, const std::vector<uint8_t>& index) {
    return [=] {
      gothicglb::writeFile(file,index.data(),index.size());
      (void)readCookedWorld(world,over,out);
      };
    };
  bytes=good;
  put32(bytes,32,get32(good,32)^1u); // the source world's CRC32
  expectThrows(readWith(dir,{},{},indexFile,bytes),{wci::FileName,"names another source world than CONTRACT.ZEN"},
               "an index of another source world");
  bytes=good;
  put64(bytes,24,get64(good,24)+1); // the source world's size alone
  expectThrows(readWith(dir,{},{},indexFile,bytes),{wci::FileName,"names another source world than CONTRACT.ZEN"},
               "an index of a source world of another size");
  const auto cellsAt=size_t(get64(good,wci::HeaderSize+16));
  bytes=good;
  bytes[cellsAt+16]^=1u; // the first byte of cell 0's GLB SHA-256
  expectThrows(readWith(dir,{},{},indexFile,bytes),{"with another GLB (length or SHA-256) than","re-cooked without the index"},
               "an index whose cell entry names another GLB than the cell's .wcb");
  bytes=good;
  bytes[cellsAt+16+31]^=1u; // the last byte of cell 0's GLB SHA-256, the first unchanged
  expectThrows(readWith(dir,{},{},indexFile,bytes),{"with another GLB (length or SHA-256) than","re-cooked without the index"},
               "an index whose cell entry differs from the cell's .wcb in the last byte of the GLB SHA-256");
  bytes=good;
  put64(bytes,cellsAt+8,get64(good,cellsAt+8)+1); // cell 0's GLB length alone
  expectThrows(readWith(dir,{},{},indexFile,bytes),{"with another GLB (length or SHA-256) than","re-cooked without the index"},
               "an index whose cell entry names a GLB of another length than the cell's .wcb");
  gothicglb::writeFile(indexFile,good.data(),good.size());

  const auto json=readText(dir/"world.json");
  const auto chunks=readWorldChunkManifest(dir).chunks;
  require(chunks.size()>=3,"the chunk fixture must have three cells or more");
  std::string fewer, swapped;
  for(size_t i=0;i<chunks.size();++i) {
    const auto quoted="\""+chunks[i]+"\"";
    if(i+1<chunks.size())
      fewer+=(i==0 ? "" : ",")+quoted;
    swapped+=(i==0 ? "" : ",")+("\""+chunks[i==0 ? 1 : i==1 ? 0 : i]+"\"");
    }
  writeText(dir/"world.json",withChunks(json,fewer));
  expectThrows([&] { (void)readCookedWorld(dir); },{"lists "+std::to_string(chunks.size())+" cells, world.json "+
               std::to_string(chunks.size()-1)},"an index with another cell count than world.json");
  writeText(dir/"world.json",withChunks(json,swapped));
  expectThrows([&] { (void)readCookedWorld(dir); },{"another cell order"},"an index with another cell order than world.json");
  writeText(dir/"world.json",json);
  require(readCookedWorld(dir).cells.size()==chunks.size(),"the restored index and world.json read again");
  // And an index of fewer cells: the strip's world without cell 0,0, read with the strip's world.json.
  const auto plainOut=root/"strip-cooked", plainIndex=root/"strip-cooked"/wci::FileName;
  const auto plainGood=fileBytes(plainIndex);
  expectThrows(readWith(root/"strip",{},plainOut,plainIndex,fileBytes(root/"strip-other-cooked"/wci::FileName)),
               {"lists 2 cells, world.json 3"},"an index with fewer cells than world.json");
  gothicglb::writeFile(plainIndex,plainGood.data(),plainGood.size());

  // Exceptions: the strip's border override gives cell 0,0 one vertex exception and cell 1,0 one
  // vertex, one feature and one material exception. Each of 1,0's locals set to the first index past its array.
  const auto strip=root/"strip", border=root/"strip-border", out=root/"strip-border-cooked";
  const auto borderFile=out/wci::FileName;
  const auto borderGood=fileBytes(borderFile);
  const auto vertexExceptionsAt=size_t(get64(borderGood,wci::HeaderSize+2*32+16));
  const auto cell=CookedChunk::read(out/"chunks/1_0.wcb");
  const std::tuple<uint32_t,size_t,size_t,const char*> past[] = {
    {3,1,cell.positions().size(),"positions"},{4,0,cell.features().size(),"features"},
    {5,0,cell.materials().size(),"materials"}};
  for(const auto& [type,entry,count,kind]:past) {
    bytes=borderGood;
    put32(bytes,size_t(get64(borderGood,wci::HeaderSize+(type-1)*32+16))+entry*8,uint32_t(count)); // local
    expectThrows(readWith(strip,border,out,borderFile,bytes),{"an exception points to local "+std::to_string(count)+
                 ", past the "+std::to_string(count)+" "+kind+" of 1_0.wcb"},
                 std::string("an exception one past a cell's ")+kind);
    }
  bytes=borderGood;
  put32(bytes,vertexExceptionsAt+4,get32(borderGood,40)); // the stitched id: the vertex total
  expectThrows(open(bytes),{"vertex exceptions","past the totals"},"a stitched id past the totals");
  // The same for 1,0's feature and material exception: the id equal to the feature and the material total.
  const std::tuple<uint32_t,size_t,const char*> totals[] = {{4,44,"feature exceptions"},{5,48,"material exceptions"}};
  for(const auto& [type,total,kind]:totals) {
    bytes=borderGood;
    put32(bytes,size_t(get64(borderGood,wci::HeaderSize+(type-1)*32+16))+4,get32(borderGood,total));
    expectThrows(open(bytes),{kind,"past the totals"},std::string("a stitched id past the totals in the ")+kind);
    }
  gothicglb::writeFile(borderFile,borderGood.data(),borderGood.size());
  require(verifyCookedWorld(strip,border,out,true).empty(),"the restored border index assembles again");

  fs::remove(indexFile);
  expectThrows([&] { (void)readCookedWorld(dir); },{wci::FileName,"is missing"},"a missing index");
  gothicglb::writeFile(indexFile,good.data(),good.size());
  std::puts("Index rejections: truncated, short, wrong magic, version, another source world by CRC32 and by size, "
            "another GLB than the cell's .wcb by the first and the last SHA-256 byte and by length, more and fewer cells and another cell order "
            "than world.json, an exception one past a cell's positions, features and materials, a vertex, feature "
            "and material id past the totals, missing");
  }

// Temporary names are unique to the process as well as the call: a forked child
// continues the parent's call count, so only the process id tells their names apart.
void temporaryNames(const fs::path& root) {
  const auto path=root/"names"/"0_0.wcb";
  require(cookedTemporaryPath(path)!=cookedTemporaryPath(path),"two calls must give two temporary names");
#ifndef _WIN32
  const auto childName=root/"child-temporary.txt";
  const pid_t child=fork();
  require(child>=0,"fork failed");
  if(child==0) {
    try {
      writeText(childName,cookedTemporaryPath(path).string());
      }
    catch(...) {
      _exit(1);
      }
    _exit(0);
    }
  const auto own=cookedTemporaryPath(path).string();
  int status=0;
  require(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0,
          "the forked process must write its temporary name");
  require(readText(childName)!=own,"two processes at the same call count must get two temporary names, both got "+own);
  std::puts("Temporary names: two calls and two processes at the same call count get different names");
#endif
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

int main(int argc, char** argv) {
  TemporaryDirectory temporary;
  try {
    require(argc==1 || (argc==2 && std::string_view(argv[1])=="--gpu"),"usage: gothic-wcb-contract [--gpu]");
    checkHiZCoverage();
    checkHistoryHiZContract();
    fs::create_directories(temporary.path);
    selectionFixtures(temporary.path);
    chunkFixtures(temporary.path);
    editedOverride(temporary.path);
    hierarchyRoundTrip(temporary.path);
    rejections(temporary.path);
    indexRejections(temporary.path);
    manifestCases(temporary.path);
    temporaryNames(temporary.path); // forks: before any other thread starts

    const auto* gothic=std::getenv("GOTHIC_DIR");
    require(gothic!=nullptr && *gothic!='\0',"GOTHIC_DIR must point to a Gothic II installation");
    const char* args[]={"gothic-wcb-contract","-g",gothic};
    CommandLine commandLine(3,args);
    Resources resources;
    Resources::loadBase();
    const auto world=Resources::worldBaseDir("NEWWORLD.ZEN");
    require(!world.empty() && fs::exists(world/"world.json"),"the base has no NEWWORLD");
    const auto label="base NEWWORLD ("+world.string()+")";
    const auto out=temporary.path/"NEWWORLD-cooked";
    concurrentCooks(world,out,label);
    const auto again=cookWorldChunks(world,{},out);
    require(again.written==0 && again.left==again.cells && !again.indexWritten,
            label+": a second cook of the unchanged world must write nothing");
    printCooked(label,again);
    std::optional<GraphicsApi> api;
    std::optional<Tempest::Device> device;
    if(argc==2) {
      api.emplace();
      device.emplace(*api);
      checkInstancedVisibilityCuts(*device);
      checkGpuHistoryDepth(*device);
      }
    size_t samples=0, gpuCells=0;
    for(const auto& file:readWorldChunkManifest(world).chunks) {
      auto path=out/file;
      path.replace_extension(".wcb");
      const auto chunk=CookedChunk::read(path,true);
      if(device && gpuCells<3 && std::ranges::any_of(chunk.hierarchy().groups,[](const ClusterLod::Group& group) {
         return group.errorCm>0.f && std::isfinite(group.errorCm);
         })) {
        const auto gpuSamples=checkVisibilityCuts(chunk,path,&*device);
        std::printf("GPU visibility cut: %zu comparisons per pipeline (main, HiZ and shadow) on %s\n",
                    gpuSamples,path.filename().string().c_str());
        samples+=gpuSamples;
        ++gpuCells;
        }
      else {
        samples+=checkVisibilityCuts(chunk,path);
        }
      }
    require(argc==1 || gpuCells==3,"GPU visibility cut requires three cooked chunks with finite transitions");
    require(samples>again.cells*16,"NEWWORLD visibility cut must exercise finite transitions");
    std::printf("Visibility cut: %zu camera/threshold comparisons with selectCut across %zu cooked NEWWORLD cells\n",
                samples,again.cells);
    std::puts("Cooked chunk contract passed");
    return EXIT_SUCCESS;
    }
  catch(const std::exception& error) {
    std::fprintf(stderr,"FAIL: %s\n",error.what());
    return EXIT_FAILURE;
    }
  }

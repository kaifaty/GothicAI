// The world cloth bundle (docs/landscape-plan.md, Д3, task 4; physics/worldclothbundle.h): WorldCloth::extract on
// a world cooked beside its GLBs takes the cooked bundle and gives what the island search gives with
// OPENGOTHIC_WCB=0 — every surface, the original mesh, the landscape and the chunk table after the removal.
//   - The synthetic cloth world (the objects of worldcloth_contract.cpp as chunks: an island across a cell
//     border, a cell with several islands, cells without cloth), three override sets (a moved cloth vertex, a new
//     island of added geometry, a removed island) and the base's NEWWORLD in a scratch copy of hard links.
//   - Every fallback gives the island search's result and its log line.
//   - The cook: a second cook writes nothing, an override writes its cells, the bundle and the index, two cooks
//     at once both finish and verify; verify-cooked-world (the function and the command) names the surface of a
//     changed pin, and the command fails.
//   - One cook per directory: a cook stopped before its index holds back a second cook of the same directory
//     until it ends, and not a cook of another directory (closes #1284).
//   - Carried over from the review of task 3: a .wcb of another source world falls back alone.
//   - A digest of searchIslands' result on the fixture, loaded with its chunk table, guards WorldCloth::Algorithm.
// Needs GOTHIC_DIR and a ready base for NEWWORLD.

#include "commandline.h"
#include "gothicglb.h"
#include "physics/worldcloth.h"
#include "physics/worldclothbundle.h"
#include "resources.h"
#include "sha256.h"
#include "worldchunkcook.h"
#include "worldchunk_fixtures.h"
#include "worldchunk_strip.h"
#include "worldchunks.h"
#include "worldcloth_fixtures.h"

#include <Tempest/Log>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
namespace fs = std::filesystem;
using namespace WorldChunkFixtures;
using Clock = std::chrono::steady_clock;

// SHA-256 of searchIslands' result on clothWorld() as chunks, with its chunk table (guardAlgorithm). Update it
// together with WorldCloth::Algorithm.
constexpr const char* resultDigest = "536af1672a01bf79dba2b5b0d2a326cb298fc41188159c9dff06273aaee94e30";

void require(bool condition, const std::string& message) {
  if(!condition)
    throw std::runtime_error(message);
  }

struct TemporaryDirectory {
  fs::path path = fs::temp_directory_path()/
    ("gothic-worldcloth-cooked-contract-"+std::to_string(Clock::now().time_since_epoch().count()));
  ~TemporaryDirectory() {
    std::error_code error;
    fs::remove_all(path,error);
    }
  };

const WorldChunkSource stripSource = {"CONTRACT.ZEN",3,0x00C0FFEEu};

// ---- extract on a loaded world, with the cooked path and with OPENGOTHIC_WCB=0 ----

struct Loaded {
  asset::WorldMesh      mesh;
  ChunkTable            table;
  WorldCloth::Extracted cloth;
  WorldCloth::Path      path = WorldCloth::Path::None;
  };

// As the game loads a world: readWorldChunks, the world's name and box as worldbase.cpp sets them, then extract.
Loaded extractLoaded(const fs::path& dir, const fs::path& over, bool cooked) {
  Loaded out;
  setCookedPath(cooked);
  try {
    out.mesh = readWorldChunks(dir,over,&out.table);
    }
  catch(...) {
    setCookedPath(true);
    throw;
    }
  setCookedPath(true);
  out.mesh.name = "CLOTH_CONTRACT_MESH";
  out.mesh.bbox = {{-1,-2,-3},{4,5,6}};
  out.cloth = WorldCloth::extract(out.mesh,&out.table);
  out.path = WorldCloth::lastPath();
  return out;
  }

std::string compareLoaded(const Loaded& e, const Loaded& a) {
  auto difference = compareWorldCloth(e.cloth,a.cloth);
  if(!difference.empty())
    return "cloth: "+difference;
  difference = compareWorldMeshes(e.mesh,a.mesh);
  if(!difference.empty())
    return "landscape: "+difference;
  difference = compareChunkTables(e.table,a.table);
  if(!difference.empty())
    return "chunk table: "+difference;
  return {};
  }

struct Outcome {
  size_t           surfaces = 0, triangles = 0;
  WorldCloth::Path path = WorldCloth::Path::None;
  std::string      log;
  };

// extract on dir with over, by the island search (OPENGOTHIC_WCB=0) and with the cooked path: equal results.
Outcome compareSet(const std::string& label, const fs::path& dir, const fs::path& over) {
  LogCapture log;
  const auto searched = extractLoaded(dir,over,false);
  require(searched.path==WorldCloth::Path::Search,label+": with OPENGOTHIC_WCB=0 extract must run the island search");
  const auto cooked = extractLoaded(dir,over,true);
  const auto difference = compareLoaded(searched,cooked);
  require(difference.empty(),label+": extract on the cooked path differs from the island search: "+difference);
  return {cooked.cloth.surfaces.size(),cooked.cloth.original.polygons.material_indices.size(),cooked.path,log.take()};
  }

void expectBundle(const std::string& label, const fs::path& dir, const fs::path& over={}) {
  const auto o = compareSet(label,dir,over);
  const auto line = "[world-cloth] bundle "+cookedClothPath(dir,over).string()+": "+std::to_string(o.surfaces)+
                    " surfaces, "+std::to_string(o.triangles)+" triangles";
  std::printf("%s: the bundle gives the island search's %zu surfaces and %zu triangles\n",label.c_str(),o.surfaces,
              o.triangles);
  require(o.path==WorldCloth::Path::Bundle,label+": extract must take the bundle; log: "+o.log);
  require(tagged(o.log,"[world-cloth] ")==1 && o.log.find(line)!=std::string::npos,
          label+": the log must hold the line '"+line+"' alone, got: "+o.log);
  }

void expectSearch(const std::string& label, const fs::path& dir, const std::string& reason, const fs::path& over={}) {
  const auto o = compareSet(label,dir,over);
  std::printf("%s: island search; log: %s\n",label.c_str(),o.log.empty() ? "(none)" : o.log.c_str());
  require(o.path==WorldCloth::Path::Search,label+": extract must run the island search");
  require(tagged(o.log,"[world-cloth] ")==(reason.empty() ? 0u : 1u),label+": the bundle must log "+
          (reason.empty() ? std::string("nothing") : "one line")+", got: "+o.log);
  require(reason.empty() || o.log.find(reason)!=std::string::npos,label+": the log must say '"+reason+"', got: "+o.log);
  }

// ---- the synthetic world and its override sets ----

void fixture(const fs::path& root, const ClothWorld& world) {
  const auto dir = root/"cloth";
  writeWorldChunks(world.mesh,clothSource,dir);
  const auto stats = cookWorldChunks(dir);
  require(stats.clothWritten && stats.clothSurfaces==4 && stats.clothTriangles==88,
          "the fixture's bundle must hold two city banners, the ship flag and the sail (88 triangles), got "+
          std::to_string(stats.clothSurfaces)+" surfaces and "+std::to_string(stats.clothTriangles)+" triangles");
  // Its shape: an island across a cell border, several islands in one cell, cells without cloth.
  const auto manifest = readWorldChunkManifest(dir);
  const auto bundle = ClothBundle::read(dir/wcl::FileName).data();
  size_t clothCells = 0, plain = 0, across = 0, inZero = 0;
  for(size_t i=0;i<manifest.cells.size();++i) {
    const auto& id = manifest.cells[i].id;
    const auto n = bundle.exclusions[i].size();
    if(n>0)
      ++clothCells;
    else
      ++plain;
    if(id==ChunkId{1,0})
      across = n;
    if(id==ChunkId{0,0})
      inZero = n;
    }
  require(manifest.cells.size()==5 && clothCells==2 && plain==3 && across==2 && inZero==86,
          "the fixture must spread its cloth over cells 0,0 (86 triangles: four islands, one of them half) and 1,0 "
          "(the other half of the banner across the border), with three cells without cloth");
  expectBundle("synthetic cloth world",dir);
  require(verifyCookedWorld(dir,{},{},true).empty(),"the synthetic cloth world must verify");

  // The override replaces cell 0,0 and moves a lower corner of its city banner.
  const auto moved = root/"cloth-moved";
  writeMovedClothCell(moved,world);
  cookWorldChunks(dir,moved);
  expectBundle("override: cell 0,0 with a moved cloth vertex",dir,moved);
  require(verifyCookedWorld(dir,moved,{},true).empty(),"the moved vertex override must verify");

  // The override adds a city banner to cell 2,0 as new geometry: new positions, no source ids or orders.
  const auto added = root/"cloth-added";
  writeAddedClothCell(added,world);
  const auto addedStats = cookWorldChunks(dir,added);
  require(addedStats.clothSurfaces==5,"the added banner must be a fifth surface, got "+
          std::to_string(addedStats.clothSurfaces));
  expectBundle("override: a new cloth island as added geometry in cell 2,0",dir,added);
  require(verifyCookedWorld(dir,added,{},true).empty(),"the added island override must verify");

  // The override drops the ship flag's triangles from cell 0,0.
  const auto removed = root/"cloth-removed";
  writeRemovedClothCell(removed,world);
  const auto removedStats = cookWorldChunks(dir,removed);
  require(removedStats.clothSurfaces==3,"without the flag three surfaces remain, got "+
          std::to_string(removedStats.clothSurfaces));
  expectBundle("override: cell 0,0 without the ship flag",dir,removed);
  require(verifyCookedWorld(dir,removed,{},true).empty(),"the removed island override must verify");
  }

// ---- fallbacks: each case in a fresh copy of the cooked fixture ----

void fallbacks(const fs::path& root, const ClothWorld& world) {
  const auto cooked = root/"fallback-cooked";
  writeWorldChunks(world.mesh,clothSource,cooked);
  cookWorldChunks(cooked);
  size_t number = 0;
  auto copy = [&] {
    const auto dir = root/("fallback-"+std::to_string(++number));
    fs::copy(cooked,dir,fs::copy_options::recursive);
    return dir;
    };

  auto dir = copy();
  fs::remove(dir/wcl::FileName);
  expectSearch("no bundle",dir,{});

  dir = copy();
  auto bytes = fileBytes(dir/wcl::FileName);
  put32(bytes,32,get32(bytes,32)^1u); // the source world's CRC32
  writeBytes(dir/wcl::FileName,bytes);
  expectSearch("a bundle of another source world",dir,"names another source world than CLOTH_CONTRACT.ZEN");

  // The set re-cooked after an edit of cell 0,0 at another length, the old bundle put back.
  dir = copy();
  const auto old = fileBytes(dir/wcl::FileName);
  writeMovedClothCell(dir,world);
  require(cookWorldChunks(dir).clothWritten,"the edit must change the bundle");
  writeBytes(dir/wcl::FileName,old);
  expectSearch("the old bundle beside the re-cooked set",dir,"was cooked with another index than");

  dir = copy();
  bytes = fileBytes(dir/wcl::FileName);
  bytes[80] = uint8_t(bytes[80]^1u); // the rules' SHA-256
  writeBytes(dir/wcl::FileName,bytes);
  expectSearch("a bundle of other rules",dir,"was cooked with other world cloth rules than this build's");

  dir = copy();
  bytes = fileBytes(dir/wcl::FileName);
  put32(bytes,44,get32(bytes,44)+1); // the algorithm
  writeBytes(dir/wcl::FileName,bytes);
  expectSearch("a bundle of another algorithm",dir,"was cooked by world cloth algorithm "+
               std::to_string(WorldCloth::Algorithm+1)+", this build runs "+std::to_string(WorldCloth::Algorithm));

  dir = copy();
  bytes = fileBytes(dir/wcl::FileName);
  bytes.pop_back();
  writeBytes(dir/wcl::FileName,bytes);
  expectSearch("a truncated bundle",dir,"truncated");

  // Exclusions edited through the codec: one past the cell's triangles, and one fewer than the original mesh has.
  const auto cells = readWorldChunkManifest(cooked).cells;
  size_t zero = cells.size();
  for(size_t i=0;i<cells.size();++i)
    if(cells[i].id==ChunkId{0,0})
      zero = i;
  require(zero<cells.size(),"the fixture has cell 0,0");
  dir = copy();
  auto data = ClothBundle::read(dir/wcl::FileName).data();
  data.exclusions[zero].back() = 1000000;
  writeBytes(dir/wcl::FileName,encodeClothBundle(data));
  expectSearch("an exclusion past a cell's triangles",dir,"cell 0,0 excludes triangle 1000000, past the cell's");

  dir = copy();
  data = ClothBundle::read(dir/wcl::FileName).data();
  data.exclusions[zero].pop_back();
  writeBytes(dir/wcl::FileName,encodeClothBundle(data));
  expectSearch("exclusions one fewer than the original mesh",dir,
               "its exclusions name 87 triangles, the loaded chunk table holds 87 of them, its original mesh has 88");

  // The set re-cooked between the world's load and extract: the new bundle is for the new index, not for the
  // landscape the load read, so the island search runs on that landscape.
  {
  dir = copy();
  LogCapture log;
  ChunkTable table;
  auto mesh = readWorldChunks(dir,{},&table);
  mesh.name = "CLOTH_CONTRACT_MESH";
  auto searchedMesh = mesh;
  auto searchedTable = table;
  writeMovedClothCell(dir,world);
  require(cookWorldChunks(dir).clothWritten,"the edit must change the bundle");
  const auto cloth = WorldCloth::extract(mesh,&table);
  const auto path = WorldCloth::lastPath();
  const auto searched = WorldCloth::searchIslands(searchedMesh,&searchedTable);
  auto difference = compareWorldCloth(searched,cloth);
  if(difference.empty())
    difference = compareWorldMeshes(searchedMesh,mesh);
  if(difference.empty())
    difference = compareChunkTables(searchedTable,table);
  const auto text = log.take();
  std::printf("the set re-cooked between load and extract: island search; log: %s\n",text.c_str());
  require(path==WorldCloth::Path::Search && difference.empty(),
          "a set re-cooked after the world's load must give the island search on the loaded landscape: "+difference);
  require(tagged(text,"[world-cloth] ")==1 && text.find("was cooked with another index than")!=std::string::npos,
          "the bundle of the re-cooked set must be refused by the index the load read, got: "+text);
  }

  // One GLB of another length at load: the set has no edits, so the index stays in use for the other cells.
  dir = copy();
  writeBytes(dir/"chunks/2_0.glb",padJson(fileBytes(dir/"chunks/2_0.glb")));
  const auto o = compareSet("a GLB of another length",dir,{});
  std::printf("a GLB of another length at load: island search; log: %s\n",o.log.c_str());
  require(o.path==WorldCloth::Path::Search && tagged(o.log,"[world-cloth] ")==1 &&
          o.log.find("bytes at world load, the index lists the cell with a GLB of")!=std::string::npos,
          "a GLB of another length must keep the bundle out with its reason, got: "+o.log);
  require(o.log.find(std::to_string(cells.size()-1)+" of "+std::to_string(cells.size())+" cells read from their .wcb")!=
          std::string::npos,"the index must stay in use for the other cells, got: "+o.log);
  }

// ---- the cook ----

void cooks(const fs::path& root, const ClothWorld& world) {
  // A second cook of an unchanged world writes nothing; an old time on its files shows any rewrite.
  const auto dir = root/"recook";
  writeWorldChunks(world.mesh,clothSource,dir);
  auto stats = cookWorldChunks(dir);
  require(stats.written==stats.cells && stats.clothWritten && stats.indexWritten,"a first cook writes everything");
  const fs::file_time_type old = std::chrono::floor<std::chrono::seconds>(fs::file_time_type::clock::now()-std::chrono::hours(24));
  for(const auto* name:{wcl::FileName,wci::FileName})
    fs::last_write_time(dir/name,old);
  stats = cookWorldChunks(dir);
  require(stats.written==0 && !stats.clothWritten && !stats.indexWritten &&
          fs::last_write_time(dir/wcl::FileName)==old && fs::last_write_time(dir/wci::FileName)==old,
          "a second cook of an unchanged world must write nothing and keep the bundle's time");
  // An override of cell 0,0: its cell, its bundle and its index are written, the base cells are left.
  const auto over = root/"recook-override";
  writeMovedClothCell(over,world);
  stats = cookWorldChunks(dir,over);
  require(stats.written==1 && stats.left==stats.cells-1 && stats.clothWritten && stats.indexWritten &&
          fs::exists(over/wcl::FileName) && fs::last_write_time(dir/wcl::FileName)==old,
          "an override must write its replaced cell, its bundle and its index, and leave the base's bundle");
  std::printf("cook: a second cook writes nothing (bundle time kept); an override writes 1 of %zu cells, the bundle "
              "and the index\n",stats.cells);

  // Two cooks into the same directory at once.
  const auto both = root/"two-cooks";
  writeWorldChunks(world.mesh,clothSource,both);
  std::string errors[2];
  auto cook = [&](int i) {
    try {
      cookWorldChunks(both);
      }
    catch(const std::exception& error) {
      errors[i] = error.what();
      }
    };
  std::thread first(cook,0), second(cook,1);
  first.join();
  second.join();
  for(const auto& error:errors)
    require(error.empty(),"one of two cooks at once failed: "+error);
  const auto difference = verifyCookedWorld(both,{},{},true);
  require(difference.empty(),"two cooks at once must leave a set that verifies: "+difference);
  expectBundle("two cooks into one directory at once",both);

  // verify-cooked-world names the surface of a changed pin.
  const auto pinned = root/"pin";
  fs::copy(both,pinned,fs::copy_options::recursive);
  auto data = ClothBundle::read(pinned/wcl::FileName).data();
  auto& surface = data.cloth.surfaces[1];
  surface.pins[0] = (surface.pins[0]+1)%uint32_t(surface.mesh.vertices.size());
  writeBytes(pinned/wcl::FileName,encodeClothBundle(data));
  const auto pin = verifyCookedWorld(pinned,{},{},true);
  require(pin.find("surface 1 ("+surface.visual+"): pins differ")!=std::string::npos,
          "verify-cooked-world must name the surface whose pin changed, got: "+pin);
  // The command fails with the same difference.
  const auto output = root/"pin-verify.txt";
  std::string command = std::string("\"")+GOTHIC_CONTENT_CONVERT+"\" verify-cooked-world \""+pinned.string()+"\" >\""+
                        output.string()+"\" 2>&1";
#ifdef _WIN32
  command = "\""+command+"\""; // cmd.exe drops the outer pair of quotes
#endif
  const int status = std::system(command.c_str());
  const auto text = readText(output);
  require(status!=0 && text.find("surface 1 ("+surface.visual+"): pins differ")!=std::string::npos,
          "content-convert verify-cooked-world must fail naming the surface whose pin changed, status "+
          std::to_string(status)+": "+text);
  std::printf("verify-cooked-world with one pin changed: %s; the command exits with status %d\n",pin.c_str(),status);
  }

// ---- one cook per directory (#1284) ----

// Stops a cook in its hook until released.
struct Gate {
  std::mutex              sync;
  std::condition_variable changed;
  bool                    stopped = false, released = false;
  void stop() {
    std::unique_lock lock(sync);
    stopped = true;
    changed.notify_all();
    changed.wait(lock,[&] { return released; });
    }
  bool waitStopped() {
    std::unique_lock lock(sync);
    return changed.wait_for(lock,std::chrono::seconds(120),[&] { return stopped; });
    }
  void release() {
    std::lock_guard lock(sync);
    released = true;
    changed.notify_all();
    }
  };

struct Cook {
  std::thread       thread;
  std::atomic<bool> finished {false};
  std::string       error;
  void start(std::function<void()> body) {
    thread = std::thread([this,body=std::move(body)] {
      try {
        body();
        }
      catch(const std::exception& e) {
        error = e.what();
        }
      finished = true;
      });
    }
  bool waitFinished(std::chrono::seconds limit) {
    const auto until = Clock::now()+limit;
    while(!finished && Clock::now()<until)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return finished;
    }
  };

// Every cell read from its .wcb equals its GLB read.
void cellsFromWcb(const std::string& label, const fs::path& dir, const fs::path& over) {
  setCookedPath(false);
  ChunkTable plain;
  const auto mesh = readWorldChunks(dir,over,&plain);
  setCookedPath(true);
  ChunkTable cooked;
  (void)readWorldChunks(dir,over,&cooked);
  const auto lists = chunkTriangleLists(mesh,plain);
  for(size_t i=0;i<lists.size();++i) {
    std::vector<uint32_t> sources;
    for(const auto t:lists[i])
      sources.push_back(plain.triangleSources[t]);
    const auto a = plain.source->read(i,sources), b = cooked.source->read(i,sources);
    const auto difference = compareWorldMeshes(a.mesh,b.mesh);
    require(difference.empty() && a.vertexIds==b.vertexIds && a.featureIds==b.featureIds,
            label+": cell "+plain.entries[i].file+" from its .wcb differs from its GLB: "+difference);
    }
  require(cooked.source->reads().cooked==lists.size() && cooked.source->reads().glb==0,
          label+": every cell must be read from its .wcb");
  }

void lock(const fs::path& root) {
  const auto base = root/"lock", over = root/"lock-override";
  writeWorldChunks(stripMesh(),stripSource,base);
  writeStripOverride(over,stripMesh(),true);
  cookWorldChunks(base,over);
  // 1_0 of the border override with its shift moved from border 4 to border 8: a GLB of the same length.
  auto moved = stripMesh();
  moved.vertices[stripVertex(8,0)].y += 30.f;
  const auto edited = writeStripOverride(root/"lock-other",moved,false)/"chunks/1_0.glb";
  require(fs::file_size(edited)==fs::file_size(over/"chunks/1_0.glb"),"the moved edit must keep its GLB's length");

  {
  Gate gate;
  Cook a, b;
  a.start([&] { cookWorldChunks(base,over,{},{},[&] { gate.stop(); }); });
  const bool stopped = gate.waitStopped();
  bool waited = false;
  if(stopped) {
    // While B waits, its GLB is unreadable (broken magic at the same length), then becomes the moved edit: a B that
    // read a GLB before taking the lock would fail on it.
    auto broken = fileBytes(edited);
    broken[0] = 'X';
    writeBytes(over/"chunks/1_0.glb",broken);
    b.start([&] { cookWorldChunks(base,over); });
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    waited = !b.finished;
    fs::copy_file(edited,over/"chunks/1_0.glb",fs::copy_options::overwrite_existing);
    }
  gate.release();
  a.thread.join();
  if(b.thread.joinable())
    b.thread.join();
  require(stopped,"cook A must reach its stop after the GLB check");
  require(waited,"cook B of the same directory must wait while A holds it; B ended with: "+
          (b.error.empty() ? std::string("success") : b.error));
  require(a.error.empty() && b.error.empty(),"both cooks must finish: A: "+a.error+"; B: "+b.error);
  const auto difference = verifyCookedWorld(base,over,{},true);
  require(difference.empty(),"after both cooks the set must verify with hashes: "+difference);
  cellsFromWcb("after two cooks of one directory",base,over);
  expectBundle("after two cooks of one directory",base,over);
  std::puts("lock: cook B waited 1.5 s while cook A was stopped before its index, reading no GLB (a broken one was "
            "replaced before A went on); both finished, the set verifies with hashes and every cell reads its .wcb "
            "equal to its GLB");
  }

  // Cooks of two directories do not wait for each other.
  {
  const auto x = root/"lock-x", y = root/"lock-y";
  writeWorldChunks(stripMesh(),stripSource,x);
  writeWorldChunks(stripMesh(),stripSource,y);
  Gate gate;
  Cook a, b;
  a.start([&] { cookWorldChunks(x,{},{},{},[&] { gate.stop(); }); });
  const bool stopped = gate.waitStopped();
  bool ran = false;
  if(stopped) {
    b.start([&] { cookWorldChunks(y); });
    ran = b.waitFinished(std::chrono::seconds(60));
    }
  gate.release();
  a.thread.join();
  if(b.thread.joinable())
    b.thread.join();
  require(stopped && ran,"a cook of another directory must finish while cook A is stopped");
  require(a.error.empty() && b.error.empty(),"both cooks must finish: A: "+a.error+"; B: "+b.error);
  require(verifyCookedWorld(x,{},{},true).empty() && verifyCookedWorld(y,{},{},true).empty(),"both sets must verify");
  std::puts("lock: a cook of another directory finished while cook A was stopped");
  }
  }

// ---- carried over from the review of task 3: a .wcb of another source world ----

void otherWorldWcb(const fs::path& root) {
  const auto dir = root/"other-world";
  writeWorldChunks(stripMesh(),stripSource,dir);
  cookWorldChunks(dir);
  auto chunk = fileBytes(dir/"chunks/1_0.wcb");
  put32(chunk,120,get32(chunk,120)^1u); // the .wcb header's CRC32 of the source world
  writeBytes(dir/"chunks/1_0.wcb",chunk);
  LogCapture log;
  setCookedPath(false);
  ChunkTable plain;
  const auto mesh = readWorldChunks(dir,{},&plain);
  setCookedPath(true);
  ChunkTable cooked;
  (void)readWorldChunks(dir,{},&cooked);
  const auto lists = chunkTriangleLists(mesh,plain);
  std::vector<std::string> glbCells;
  for(size_t i=0;i<lists.size();++i) {
    std::vector<uint32_t> sources;
    for(const auto t:lists[i])
      sources.push_back(plain.triangleSources[t]);
    const auto before = cooked.source->reads().glb;
    const auto a = plain.source->read(i,sources), b = cooked.source->read(i,sources);
    const auto difference = compareWorldMeshes(a.mesh,b.mesh);
    require(difference.empty() && a.vertexIds==b.vertexIds && a.featureIds==b.featureIds,
            "a .wcb of another source world: cell "+plain.entries[i].file+" differs from its GLB: "+difference);
    if(cooked.source->reads().glb!=before)
      glbCells.push_back(plain.entries[i].file);
    }
  const auto text = log.take();
  require(glbCells==std::vector<std::string>{"chunks/1_0.glb"},"only 1_0 must read its GLB");
  require(tagged(text,"[world-chunks] ")==2 && text.find("1_0.wcb was cooked for another source world than CONTRACT.ZEN")!=
          std::string::npos,"the cell must log that its .wcb was cooked for another source world, got: "+text);
  std::printf("a .wcb of another source world: only 1_0 reads its GLB, equal; log: %s\n",text.c_str());
  }

// ---- the base's NEWWORLD in a scratch copy ----

void newWorld(const fs::path& root, const fs::path& world) {
  const auto scratch = root/"NEWWORLD";
  const auto inBase = cookedFiles(world);
  linkWorld(world,scratch);
  const auto started = Clock::now();
  const auto stats = cookWorldChunks(scratch);
  std::printf("NEWWORLD cooked beside its GLBs in %s: %zu cells, bundle %llu bytes with %zu surfaces and %llu "
              "triangles, %.1f s\n",scratch.string().c_str(),stats.cells,static_cast<unsigned long long>(stats.clothBytes),
              stats.clothSurfaces,static_cast<unsigned long long>(stats.clothTriangles),
              std::chrono::duration<double>(Clock::now()-started).count());
  require(cookedFiles(world)==inBase,"the base must not be written");
  require(stats.clothSurfaces>0,"NEWWORLD has world cloth");
  expectBundle("base NEWWORLD",scratch);
  }

// ---- the guard of WorldCloth::Algorithm ----

struct Digest {
  Sha256 hash;
  void u32(uint32_t v) {
    uint8_t b[4];
    for(unsigned i=0;i<4;++i)
      b[i] = uint8_t(v>>(8*i));
    hash.update(b,4);
    }
  void f32(float v) { u32(std::bit_cast<uint32_t>(v)); }
  void text(const std::string& s) {
    u32(uint32_t(s.size()));
    hash.update(s.data(),s.size());
    }
  void polygons(const asset::PolygonList& p) {
    u32(uint32_t(p.material_indices.size()));
    for(size_t t=0;t<p.material_indices.size();++t) {
      u32(p.material_indices[t]);
      u32(uint32_t(p.lightmap_indices[t]));
      u32(wcb::packFlags(p.flags[t]));
      u32(uint32_t(uint16_t(p.flags[t].sector_index)));
      for(unsigned c=0;c<3;++c) {
        u32(p.vertex_indices[t*3+c]);
        u32(p.feature_indices[t*3+c]);
        }
      }
    }
  void mesh(const asset::WorldMesh& m) {
    text(m.name);
    for(const auto* v:{&m.bbox.min,&m.bbox.max}) {
      f32(v->x); f32(v->y); f32(v->z);
      }
    u32(uint32_t(m.materials.size()));
    for(const auto& material:m.materials)
      text(gothicglb::materialJson(material,0));
    u32(uint32_t(m.vertices.size()));
    for(const auto& v:m.vertices) {
      f32(v.x); f32(v.y); f32(v.z);
      }
    u32(uint32_t(m.features.size()));
    for(const auto& f:m.features) {
      f32(f.texture.x); f32(f.texture.y); u32(f.light); f32(f.normal.x); f32(f.normal.y); f32(f.normal.z);
      }
    polygons(m.polygons);
    }
  };

// On the game's input: the fixture written as chunks and read back with its chunk table, which the result changes.
void guardAlgorithm(const fs::path& root, const ClothWorld& world) {
  const auto dir = root/"guard";
  writeWorldChunks(world.mesh,clothSource,dir);
  ChunkTable table;
  auto landscape = readWorldChunks(dir,{},&table);
  landscape.name = "CLOTH_CONTRACT_MESH";
  const auto cloth = WorldCloth::searchIslands(landscape,&table);
  Digest d;
  d.u32(uint32_t(cloth.surfaces.size()));
  for(const auto& s:cloth.surfaces) {
    d.u32(s.id);
    d.text(s.visual);
    d.f32(s.stiffness);
    d.f32(s.linearDamping);
    d.u32(uint32_t(s.pins.size()));
    for(const auto pin:s.pins)
      d.u32(pin);
    d.mesh(s.mesh);
    }
  d.mesh(cloth.original);
  d.polygons(landscape.polygons);
  d.u32(uint32_t(table.entries.size()));
  for(const auto& entry:table.entries) {
    d.u32(entry.triangleCount);
    for(const auto* v:{&entry.min,&entry.max}) {
      d.f32(v->x); d.f32(v->y); d.f32(v->z);
      }
    }
  d.u32(uint32_t(table.triangleChunks.size()));
  for(size_t t=0;t<table.triangleChunks.size();++t) {
    d.u32(table.triangleChunks[t]);
    d.u32(table.triangleSources[t]);
    }
  const auto value = d.hash.finish();
  std::printf("searchIslands on the fixture: %zu surfaces, digest %s\n",cloth.surfaces.size(),value.c_str());
  require(value==resultDigest,"extract's result changed: raise WorldCloth::Algorithm and update this digest (now "+
          value+")");
  }

}

int main() {
  TemporaryDirectory temporary;
  try {
    fs::create_directories(temporary.path);
    const auto world = clothWorld();
    fixture(temporary.path,world);
    fallbacks(temporary.path,world);
    cooks(temporary.path,world);
    lock(temporary.path);
    otherWorldWcb(temporary.path);

    const auto* gothic = std::getenv("GOTHIC_DIR");
    require(gothic!=nullptr && *gothic!='\0',"GOTHIC_DIR must point to a Gothic II installation");
    const char* args[] = {"gothic-worldcloth-cooked-contract","-g",gothic};
    CommandLine commandLine(3,args);
    Resources resources;
    Resources::loadBase();
    const auto base = Resources::worldBaseDir("NEWWORLD.ZEN");
    require(!base.empty() && fs::exists(base/"world.json"),"the base has no NEWWORLD");
    newWorld(temporary.path,base);

    guardAlgorithm(temporary.path,world);
    std::puts("World cloth bundle contract passed");
    return EXIT_SUCCESS;
    }
  catch(const std::exception& error) {
    std::fprintf(stderr,"FAIL: %s\n",error.what());
    return EXIT_FAILURE;
    }
  }

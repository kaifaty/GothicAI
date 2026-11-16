// The catalogue start (docs/landscape-plan.md, Д3, task 8; worldchunkcatalogue.h): with streaming on and a cooked
// set the world load reads no GLB and stitches nothing, and gives what the stitched start gives.
//   - On the cooked chunk contract's fixtures, the strip with its edits, the synthetic cloth world with its three
//     edits and the base's NEWWORLD cooked into a scratch copy (hard links; the base is never written): the
//     catalogue start's chunk table (cells, files, meshes, counts and bounds bit for bit after world cloth), material
//     table and packing representatives, the materials the streaming view decodes in their order, the landscape's
//     bounds, WorldCloth::extract's result, static collision's input and every cell of the ring (its triangles in
//     stitched order with their ids) equal the stitched start's (readWorldChunks and extract).
//   - Every case that refuses the start gives the stitched start's result and its own line; a cell whose .wcb fails
//     after the start and a collision builder that gives nothing read the world then, once, with the same result.
// Needs GOTHIC_DIR and a ready base for NEWWORLD.

#include "commandline.h"
#include "graphics/landscapestart.h"
#include "graphics/mesh/submesh/packedmesh.h"
#include "physics/worldcloth.h"
#include "physics/worldclothbundle.h"
#include "physics/worldcollisionmesh.h"
#include "resources.h"
#include "worldchunkcatalogue.h"
#include "worldchunkcook.h"
#include "worldchunk_fixtures.h"
#include "worldchunk_strip.h"
#include "worldchunks.h"
#include "worldcloth_fixtures.h"

#include <Tempest/Log>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {
namespace fs = std::filesystem;
using namespace WorldChunkFixtures;
using Clock = std::chrono::steady_clock;

void require(bool condition, const std::string& message) {
  if(!condition)
    throw std::runtime_error(message);
  }

struct TemporaryDirectory {
  fs::path path = fs::temp_directory_path()/
    ("gothic-world-catalogue-contract-"+std::to_string(Clock::now().time_since_epoch().count()));
  ~TemporaryDirectory() {
    std::error_code error;
    fs::remove_all(path,error);
    }
  };

const WorldChunkSource fixtureSource = {"CONTRACT.ZEN",3,0x00C0FFEEu};

void setEnv(const char* name, const char* value) {
#ifdef _WIN32
  _putenv_s(name,value==nullptr ? "" : value);
#else
  if(value==nullptr)
    unsetenv(name);
  else
    setenv(name,value,1);
#endif
  }

double msSince(Clock::time_point start) {
  return std::chrono::duration<double,std::milli>(Clock::now()-start).count();
  }

bool sameBits(const asset::Vec3& a, const asset::Vec3& b) {
  for(unsigned axis=0;axis<3;++axis)
    if(std::bit_cast<uint32_t>(a[axis])!=std::bit_cast<uint32_t>(b[axis]))
      return false;
  return true;
  }

bool sameBits(const Tempest::Vec3& a, const Tempest::Vec3& b) {
  return sameBits(asset::Vec3{a.x,a.y,a.z},asset::Vec3{b.x,b.y,b.z});
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

// A world load up to the streaming view: the start, then WorldCloth::extract, as World's constructor runs them.
struct Start {
  asset::WorldMesh      landscape;
  ChunkTable            table;
  WorldCloth::Extracted cloth;
  bool                  clothBundle = false;
  std::string           log;
  double                ms = 0;
  const WorldCatalogue* catalogue() const { return table.source ? table.source->catalogue() : nullptr; }
  };

std::unique_ptr<Start> start(const fs::path& dir, const fs::path& over, bool catalogue) {
  LogCapture log;
  auto s=std::make_unique<Start>();
  const auto started=Clock::now();
  const auto manifest=readWorldChunkManifest(dir);
  s->landscape=catalogue ? startWorldChunks(dir,over,manifest,s->table,true) : readWorldChunks(dir,over,&s->table);
  s->cloth=WorldCloth::extract(s->landscape,&s->table);
  s->ms=msSince(started);
  s->clothBundle=WorldCloth::lastPath()==WorldCloth::Path::Bundle;
  s->log=log.take();
  return s;
  }

// Everything the streaming load takes from a start; the stitched start's is expected. Collision goes first, as at
// load: its fallback leaves the stitched read the cells' fallback reuses.
void compareStarts(const std::string& label, Start& expected, Start& actual) {
  auto fail=[&](const std::string& what) { throw std::runtime_error(label+": "+what); };
  const auto& e=expected.table.entries;
  const auto& a=actual.table.entries;
  if(e.size()!=a.size())
    fail(std::to_string(a.size())+" cells, the stitched start "+std::to_string(e.size()));
  for(size_t i=0;i<e.size();++i)
    if(!(e[i].id==a[i].id) || e[i].file!=a[i].file || !(e[i].meshes==a[i].meshes) ||
       e[i].triangleCount!=a[i].triangleCount || !sameBits(e[i].min,a[i].min) || !sameBits(e[i].max,a[i].max))
      fail("chunk table entry "+std::to_string(i)+" ("+e[i].file+"): "+std::to_string(a[i].triangleCount)+" vs "+
           std::to_string(e[i].triangleCount)+" triangles, or another cell, file, mesh list or bounds");
  if(expected.landscape.materials.size()!=actual.landscape.materials.size())
    fail("material tables of "+std::to_string(actual.landscape.materials.size())+" and "+
         std::to_string(expected.landscape.materials.size()));
  for(size_t m=0;m<expected.landscape.materials.size();++m)
    if(!gothicglb::sameMaterial(expected.landscape.materials[m],actual.landscape.materials[m]))
      fail("material "+std::to_string(m)+" differs");
  if(expected.landscape.name!=actual.landscape.name)
    fail("the landscape's name differs");
  if(PackedMesh::landscapeMaterialMap(expected.landscape)!=PackedMesh::landscapeMaterialMap(actual.landscape))
    fail("the packing representatives differ");
  // What streamingWorldView decodes and the bounds it gives the view (graphics/landscapestart.h).
  const auto es=landscapeStart(expected.landscape,expected.table), as=landscapeStart(actual.landscape,actual.table);
  if(es.materials!=as.materials)
    fail("the streaming view decodes other materials or in another order");
  if(!sameBits(es.bounds.first,as.bounds.first) || !sameBits(es.bounds.second,as.bounds.second))
    fail("the landscape's bounds differ");
  if(expected.clothBundle!=actual.clothBundle)
    fail("WorldCloth::extract took the bundle on one start only");
  auto difference=compareWorldCloth(expected.cloth,actual.cloth);
  if(!difference.empty())
    fail("world cloth: "+difference);

  WorldCollisionSource ec(expected.table,expected.clothBundle,expected.landscape);
  WorldCollisionSource ac(actual.table,actual.clothBundle,actual.landscape);
  difference=compareWorldMeshes(ec(),ac());
  if(!difference.empty())
    fail("static collision's input: "+difference);
  if(ec.probe().find(" input=chunks")!=ac.probe().find(" input=chunks"))
    fail("static collision's input came from "+ac.probe()+", on the stitched start from "+ec.probe());

  const auto eLists=chunkSourceLists(expected.table), aLists=chunkSourceLists(actual.table);
  for(size_t i=0;i<e.size();++i) {
    const auto want=expected.table.source->read(i,eLists.at(i));
    const auto got=actual.catalogue() ? actual.table.source->readLandscape(i) : actual.table.source->read(i,aLists.at(i));
    difference=compareReads(want,got);
    if(!difference.empty())
      fail("cell "+e[i].file+": "+difference);
    }
  }

// The catalogue start of a cooked set equals the stitched start, logs its one line, and reads every cell of the ring
// from its .wcb. Returns both starts' times.
std::pair<double,double> expectCatalogue(const std::string& label, const fs::path& dir, const fs::path& over={}) {
  const auto stitched=start(dir,over,false);
  const auto catalogue=start(dir,over,true);
  require(catalogue->catalogue()!=nullptr,label+": the catalogue start was not taken: "+catalogue->log);
  require(stitched->catalogue()==nullptr && !stitched->table.triangleChunks.empty(),label+": the stitched start");
  require(tagged(catalogue->log,"[world-catalogue] ")==1 &&
          catalogue->log.find("cells from the index and the world cloth bundle")!=std::string::npos,
          label+": the catalogue start must log its one line, logged: "+catalogue->log);
  require(catalogue->table.triangleChunks.empty() && catalogue->table.triangleSources.empty() &&
          catalogue->landscape.polygons.material_indices.empty() && catalogue->landscape.vertices.empty(),
          label+": the catalogue start must hold no geometry");
  require(loadedWorldTriangles(catalogue->landscape,catalogue->table)==
          loadedWorldTriangles(stitched->landscape,stitched->table)+stitched->cloth.original.polygons.material_indices.size(),
          label+": the catalogue start must count the stitched mesh's triangles");
  LogCapture log;
  compareStarts(label,*stitched,*catalogue);
  const auto reads=catalogue->table.source->reads();
  const auto lines=log.take();
  require(reads.cooked==catalogue->table.entries.size() && reads.glb==0,label+": "+std::to_string(reads.cooked)+
          " cells from their .wcb and "+std::to_string(reads.glb)+" from their GLB");
  require(tagged(lines,"[world-catalogue] ")==0,label+": no fallback may run, logged: "+lines);
  std::printf("%s: the catalogue start equals the stitched start on %zu cells; start %.1f ms against %.1f ms\n",
              label.c_str(),catalogue->table.entries.size(),catalogue->ms,stitched->ms);
  return {catalogue->ms,stitched->ms};
  }

// A case that refuses the catalogue start: the stitched result, with lines catalogue lines naming reason.
void expectStitched(const std::string& label, const fs::path& dir, const fs::path& over, size_t lines,
                    const std::string& reason={}) {
  const auto stitched=start(dir,over,false);
  const auto refused=start(dir,over,true);
  std::printf("%s: %s\n",label.c_str(),refused->log.c_str());
  require(refused->catalogue()==nullptr,label+": the catalogue start must not be taken");
  require(tagged(refused->log,"[world-catalogue] ")==lines,label+": "+std::to_string(lines)+" catalogue lines expected, "
          "logged: "+refused->log);
  require(reason.empty() || refused->log.find(reason)!=std::string::npos,label+": the line must say '"+reason+"'");
  compareStarts(label,*stitched,*refused);
  }

// ---- the fixtures, cooked beside their GLBs ----

void fixtureSets(const fs::path& root) {
  const auto selection=root/"selection";
  writeWorldChunks(selectionMesh(),fixtureSource,selection);
  cookWorldChunks(selection);
  expectCatalogue("selection",selection);

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
    expectCatalogue(dir==&chunks ? "chunk fixture" : "chunk fixture as objects with unreadable extra files",*dir);
    }

  // The strip with an edit of 1_0, the border edit and both 1_0 and 2_0 edited.
  const auto base=stripMesh();
  const auto strip=root/"strip";
  writeWorldChunks(base,fixtureSource,strip);
  cookWorldChunks(strip);
  expectCatalogue("strip",strip);
  const auto edit=writeStripOverride(root/"strip-edit",base,false);
  const auto border=writeStripOverride(root/"strip-border",base,true);
  const auto second=writeStripOverride(root/"strip-second",base,false,{2,0});
  const auto both=root/"strip-both";
  fs::create_directories(both/"chunks");
  fs::copy_file(edit/"chunks/1_0.glb",both/"chunks/1_0.glb");
  fs::copy_file(second/"chunks/2_0.glb",both/"chunks/2_0.glb");
  for(const auto* over:{&edit,&border,&both}) {
    cookWorldChunks(strip,*over);
    expectCatalogue("strip with the override "+over->filename().string(),strip,*over);
    }

  // The cloth world: islands across a cell border and several in a cell, with each of its three edits.
  const auto world=clothWorld();
  const auto cloth=root/"cloth";
  writeWorldChunks(world.mesh,clothSource,cloth);
  cookWorldChunks(cloth);
  expectCatalogue("cloth world",cloth);
  const std::pair<const char*,void(*)(const fs::path&,const ClothWorld&)> edits[] = {
    {"moved",writeMovedClothCell},{"added",writeAddedClothCell},{"removed",writeRemovedClothCell}};
  for(const auto& [name,write]:edits) {
    const auto over=root/(std::string("cloth-")+name);
    write(over,world);
    cookWorldChunks(cloth,over);
    expectCatalogue(std::string("cloth world with the ")+name+" edit",cloth,over);
    }
  }

// ---- the cases that refuse the start, each in a fresh copy of the cooked cloth world with its added edit ----

void refusals(const fs::path& root) {
  const auto world=clothWorld();
  const auto cooked=root/"refusal-cooked", over=root/"refusal-cooked-added";
  writeWorldChunks(world.mesh,clothSource,cooked);
  writeAddedClothCell(over,world);
  cookWorldChunks(cooked,over);
  size_t number=0;
  // A copy of the base and the override; the set's index and bundle lie in the override.
  auto copy=[&] {
    const auto dir=root/("refusal-"+std::to_string(++number));
    fs::create_directories(dir);
    fs::copy(cooked,dir/"base",fs::copy_options::recursive);
    fs::copy(over,dir/"over",fs::copy_options::recursive);
    return std::pair{dir/"base",dir/"over"};
    };

  fs::path dir, set;
  std::tie(dir,set)=copy();
  fs::remove(set/wci::FileName);
  expectStitched("no index",dir,set,0);

  std::tie(dir,set)=copy();
  for(const auto* flag:{"OPENGOTHIC_WCB","OPENGOTHIC_WORLD_CATALOGUE","OPENGOTHIC_STREAMING"}) {
    setEnv(flag,"0");
    try {
      expectStitched(std::string(flag)+"=0",dir,set,0);
      }
    catch(...) {
      setEnv(flag,nullptr);
      throw;
      }
    setEnv(flag,nullptr);
    }

  std::tie(dir,set)=copy();
  writeBytes(dir/"chunks/0_0.glb",padJson(fileBytes(dir/"chunks/0_0.glb")));
  expectStitched("a GLB of another length than the index's",dir,set,1,"bytes, the index lists the cell with a GLB of");

  std::tie(dir,set)=copy();
  auto bytes=fileBytes(set/wci::FileName);
  put32(bytes,32,get32(bytes,32)^1u); // the source world's CRC32
  writeBytes(set/wci::FileName,bytes);
  expectStitched("an index of another world",dir,set,1,"names another source world");

  std::tie(dir,set)=copy();
  fs::remove(dir/"chunks/0_0.wcb");
  expectStitched("a missing .wcb",dir,set,1,"0_0.wcb is missing");

  std::tie(dir,set)=copy();
  fs::remove(set/wcl::FileName);
  expectStitched("no world cloth bundle",dir,set,1,"the world cloth bundle is missing");

  std::tie(dir,set)=copy();
  bytes=fileBytes(set/wcl::FileName);
  bytes[80]=uint8_t(bytes[80]^1u); // the rules' SHA-256
  writeBytes(set/wcl::FileName,bytes);
  expectStitched("a bundle of other rules",dir,set,1,"other world cloth rules");

  std::tie(dir,set)=copy();
  bytes=fileBytes(set/wcl::FileName);
  bytes[48]=uint8_t(bytes[48]^1u); // the index's SHA-256
  writeBytes(set/wcl::FileName,bytes);
  expectStitched("a bundle of another index",dir,set,1,"was cooked with another index");

  std::tie(dir,set)=copy();
  bytes=fileBytes(set/wcl::FileName);
  put32(bytes,4,1); // a bundle of version 1 holds no cells' catalogue
  writeBytes(set/wcl::FileName,bytes);
  expectStitched("a bundle of version 1",dir,set,1,"version");

  // After the start: 0_0.wcb broken at its end. The collision builder gives nothing and reads the world; the cell
  // falls back to its GLB against that read's ids; both equal the stitched start on the same files.
  std::tie(dir,set)=copy();
  bytes=fileBytes(dir/"chunks/0_0.wcb");
  bytes.pop_back();
  writeBytes(dir/"chunks/0_0.wcb",bytes);
  {
  const auto stitched=start(dir,set,false);
  const auto catalogue=start(dir,set,true);
  require(catalogue->catalogue()!=nullptr,"a truncated .wcb is found when it is read, not at the start");
  LogCapture log;
  compareStarts("a .wcb truncated after the start",*stitched,*catalogue);
  const auto lines=log.take();
  std::printf("a .wcb truncated after the start: %s\n",lines.c_str());
  require(tagged(lines,"[world-catalogue] ")==1 && lines.find("read and stitched for a fallback")!=std::string::npos,
          "one stitched read must serve the collision and the cell, logged: "+lines);
  require(lines.find("0_0.wcb not used, the cell reads its GLB")!=std::string::npos &&
          lines.find("the collision comes from the stitched landscape")!=std::string::npos,
          "the cell and the collision must log their fallbacks, logged: "+lines);
  const auto reads=catalogue->table.source->reads();
  require(reads.glb==1 && reads.cooked+1==catalogue->table.entries.size(),"only 0_0 may read its GLB");
  }

  // After the start the set no longer fits the bundle: 0_0.glb replaced at the index's length by the cell without the
  // ship flag, its .wcb truncated. The collision fallback runs the island search on its stitched read, as the start
  // without the bundle does, instead of failing the load; the cell then reads its GLB against that read, no second one.
  {
  const auto edited=root/"refusal-changed";
  writeWorldChunks(world.mesh,clothSource,edited);
  writeRemovedClothCell(root/"refusal-changed-edit",world);
  auto original=fileBytes(edited/"chunks/0_0.glb"), changed=fileBytes(root/"refusal-changed-edit/chunks/0_0.glb");
  require(original.size()%4==0 && changed.size()%4==0,"GLBs are 4-byte aligned");
  while(original.size()<changed.size())
    original=padJson(original);
  while(changed.size()<original.size())
    changed=padJson(changed);
  writeBytes(edited/"chunks/0_0.glb",original);
  cookWorldChunks(edited);
  writeBytes(edited/"chunks/0_0.glb",changed);
  bytes=fileBytes(edited/"chunks/0_0.wcb");
  bytes.pop_back();
  writeBytes(edited/"chunks/0_0.wcb",bytes);
  setEnv("OPENGOTHIC_WCB","0");
  const auto plain=start(edited,{},false);
  setEnv("OPENGOTHIC_WCB",nullptr);
  const auto catalogue=start(edited,{},true);
  require(catalogue->catalogue()!=nullptr,"a GLB changed at the same length is not noticed at the start");
  LogCapture log;
  WorldCollisionSource ec(plain->table,plain->clothBundle,plain->landscape);
  WorldCollisionSource ac(catalogue->table,catalogue->clothBundle,catalogue->landscape);
  const auto difference=compareWorldMeshes(ec(),ac());
  require(difference.empty(),"the collision fallback must give the island search's landscape: "+difference);
  const auto& entries=catalogue->table.entries;
  const auto cell=size_t(std::find_if(entries.begin(),entries.end(),[](const auto& e) { return e.id==ChunkId{0,0}; })-
                         entries.begin());
  (void)catalogue->table.source->readLandscape(cell);
  const auto lines=log.take();
  std::printf("a set changed after the start: %s\n",lines.c_str());
  require(lines.find("does not fit the stitched read, the island search runs")!=std::string::npos &&
          tagged(lines,"[world-catalogue] ")==2,"one stitched read with the island search must serve both, logged: "+lines);
  }

  // A stitched read that fails is not repeated: 0_0.glb unreadable at its length and its .wcb truncated; the cell's
  // second read throws the kept failure even after the GLB is back.
  {
  std::tie(dir,set)=copy();
  bytes=fileBytes(dir/"chunks/0_0.wcb");
  bytes.pop_back();
  writeBytes(dir/"chunks/0_0.wcb",bytes);
  const auto glb=fileBytes(dir/"chunks/0_0.glb");
  auto broken=glb;
  broken[0]='X';
  writeBytes(dir/"chunks/0_0.glb",broken);
  const auto catalogue=start(dir,set,true);
  require(catalogue->catalogue()!=nullptr,"a broken GLB of the index's length is not noticed at the start");
  const auto& entries=catalogue->table.entries;
  const auto cell=size_t(std::find_if(entries.begin(),entries.end(),[](const auto& e) { return e.id==ChunkId{0,0}; })-
                         entries.begin());
  std::string errors[2];
  for(auto& error:errors) {
    try {
      (void)catalogue->table.source->readLandscape(cell);
      }
    catch(const std::exception& e) {
      error=e.what();
      }
    writeBytes(dir/"chunks/0_0.glb",glb);
    }
  std::printf("a failed stitched read: %s; again: %s\n",errors[0].c_str(),errors[1].c_str());
  require(errors[0].find("the stitched read of the fallback failed")!=std::string::npos && errors[1]==errors[0],
          "the second read must throw the kept failure without reading again");
  }
  }

// ---- the base's NEWWORLD in a scratch copy ----

void newWorld(const fs::path& root, const fs::path& world) {
  const auto scratch=root/"NEWWORLD";
  const auto inBase=cookedFiles(world);
  linkWorld(world,scratch);
  const auto started=Clock::now();
  const auto stats=cookWorldChunks(scratch);
  std::printf("NEWWORLD cooked beside its GLBs: %zu cells, %zu cloth surfaces, %.1f s\n",stats.cells,stats.clothSurfaces,
              std::chrono::duration<double>(Clock::now()-started).count());
  require(cookedFiles(world)==inBase,"the base must not be written");
  expectCatalogue("base NEWWORLD",scratch);
  }

}

int main() {
  TemporaryDirectory temporary;
  try {
    fs::create_directories(temporary.path);
    fixtureSets(temporary.path);
    refusals(temporary.path);

    const auto* gothic=std::getenv("GOTHIC_DIR");
    require(gothic!=nullptr && *gothic!='\0',"GOTHIC_DIR must point to a Gothic II installation");
    const char* args[]={"gothic-world-catalogue-contract","-g",gothic};
    CommandLine commandLine(3,args);
    Resources resources;
    Resources::loadBase();
    const auto world=Resources::worldBaseDir("NEWWORLD.ZEN");
    require(!world.empty() && fs::exists(world/"world.json"),"the base has no NEWWORLD");
    newWorld(temporary.path,world);
    std::puts("World catalogue start contract passed");
    return EXIT_SUCCESS;
    }
  catch(const std::exception& error) {
    std::fprintf(stderr,"FAIL: %s\n",error.what());
    return EXIT_FAILURE;
    }
  }

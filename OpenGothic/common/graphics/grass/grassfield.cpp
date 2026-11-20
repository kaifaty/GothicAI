#include "grassfield.h"

#include <Tempest/Device>
#include <Tempest/Log>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

#include "graphics/grass/grassdiag.h"
#include "graphics/sceneglobals.h"
#include "graphics/shaders.h"
#include "utils/workers.h"
#include "resources.h"

using namespace Tempest;

namespace {

constexpr int32_t ChunkCells = ChunkId::Size/int32_t(Grass::CellSize/Grass::Unit);
static_assert(ChunkId::Size%(Grass::CellSize/Grass::Unit)==0, "a chunk is a whole number of cells");

// the camera's distance at which a cell takes its next level, cm: of Grass::Ring::full; the first is the whole
constexpr float LevelFrom[] = {0.f, 1.2f, 2.f, 3.2f, 4.8f};
constexpr float LevelLead   = 200.f; // cm: more blades are asked for this early
constexpr float LevelSlack  = 600.f; // and fewer this late

constexpr size_t  UploadBytes  = 512*1024; // a frame; one cell at least
constexpr size_t  MaxJobs      = 6;

int32_t floorDiv(int32_t a, int32_t b) {
  const int32_t q = a/b;
  return (a%b!=0 && ((a<0)!=(b<0))) ? q-1 : q;
  }

}

GrassField::GrassField(SceneGlobals& scene, std::vector<ChunkId> ids, const std::filesystem::path& worldContentDir)
  :scene(scene) {
  const auto sw = Grass::enabled();
  ring   = Grass::ring(sw.mode);
  blades = sw.blades;
  for(auto& i:ids)
    world.insert(key(i.x,i.z));

  packBuffers.front() = Resources::device().ibo(Grass::packIndices(1));

  // six more maps a ground: with bindless textures alone (DrawBuckets::commit)
  layers = Shaders::options().doBindless;
  scene.setGrassRing(Vec4(ring.full, ring.fadeFrom, ring.radius, blades ? 1.f : 0.5f));
  if(blades || layers) {
    {
    std::lock_guard guard(sync);
    plots.load(worldContentDir, [this](std::string_view material) {
      asset::Material m;
      m.texture = std::string(material);
      return slotOf(m);
      });
    }
    worker = std::thread([this] { run(); });
    }
  }

GrassField::~GrassField() {
  {
  std::lock_guard guard(sync);
  stopping = true;
  }
  wake.notify_one();
  if(worker.joinable())
    worker.join();
  scene.setGrassRing(Vec4());
  scene.setGrassDrawn(false);
  scene.grassBorder = nullptr;
  if(!borderCpu.empty())
    Resources::recycle(std::move(borderGpu));
  for(auto& i:spanGpu)
    if(i.byteSize()!=0)
      Resources::recycle(std::move(i));
  }

uint32_t GrassField::rankLimit(uint8_t level) const {
  if(level==0)
    return 256;
  return uint32_t(std::min(256.f, std::ceil(256.f*RankShare/LevelFrom[level])));
  }

uint8_t GrassField::levelAt(float dist) const {
  uint8_t l = 0;
  for(uint8_t i=1; i<std::size(LevelFrom); ++i)
    if(dist>=LevelFrom[i]*ring.full)
      l = i;
  return l;
  }

uint16_t GrassField::slotOf(const asset::Material& m) {
  std::string name = m.texture;
  for(auto& c:name)
    c = char(std::toupper(static_cast<unsigned char>(c)));
  if(const auto it = slotByTexture.find(name); it!=slotByTexture.end())
    return it->second;
  auto params = Resources::grassParams(name);
  if(params.on && layers) {
    // the ground's own layers: the sward lies by the cover's rule, a blade takes its colour from the sward's map
    if(const auto g = Resources::groundParams(name); g.ground.on) {
      params.cover = true;
      params.tile  = g.ground.swardTile*100.f;
      params.fine  = params.fine && !Grass::fineOff();
      if(const int32_t forced = Grass::forcedBroad(); forced>=0)
        params.broad = float(forced)*0.01f;
      }
    }
  uint16_t id = uint16_t(-1);
  if(params.on && slots.size()<0xFFFF) {
    id = uint16_t(slots.size());
    Slot s;
    s.material = m;
    s.params   = params;
    slots.push_back(std::move(s));
    }
  slotByTexture.emplace(std::move(name),id);
  return id;
  }

GrassField::VergeOf GrassField::vergeOf(const asset::Material& m) {
  std::string name = m.texture;
  for(auto& c:name)
    c = char(std::toupper(static_cast<unsigned char>(c)));
  if(const auto it = vergeByTexture.find(name); it!=vergeByTexture.end())
    return it->second;
  VergeOf ret;
  if(const auto g = Resources::groundParams(name); g.verge.on) {
    asset::Material grass;
    grass.texture = g.of;
    ret.slot   = slotOf(grass);
    ret.reach  = g.verge.reach;
    ret.margin = g.verge.margin;
    }
  vergeByTexture.emplace(std::move(name),ret);
  return ret;
  }

void GrassField::addChunk(ChunkId id, const asset::WorldMesh& mesh) {
  if(!blades && !layers)
    return;
  auto  summary = std::make_shared<Grass::Summary>();
  auto& p       = mesh.polygons;

  std::vector<uint16_t> slot(mesh.materials.size(), uint16_t(-1));
  std::vector<bool>     water(mesh.materials.size(), false);
  std::vector<VergeOf>  verge(mesh.materials.size());
  {
  std::lock_guard guard(sync);
  for(size_t i=0; i<mesh.materials.size(); ++i) {
    water[i] = mesh.materials[i].group==asset::MaterialGroup::WATER;
    if(!water[i])
      slot[i] = slotOf(mesh.materials[i]);
    if(layers && !water[i] && slot[i]==uint16_t(-1))
      verge[i] = vergeOf(mesh.materials[i]);
    }
  }

  // the corners of the grass: other ground that shares one lies beside it
  std::vector<bool> corner(mesh.vertices.size(), false);
  for(size_t i=0; i<p.material_indices.size(); ++i) {
    const uint32_t m = p.material_indices[i];
    if(m<slot.size() && slot[m]!=uint16_t(-1))
      for(size_t k=0; k<3; ++k)
        corner[p.vertex_indices[i*3+k]] = true;
    }

  for(size_t i=0; i<p.material_indices.size(); ++i) {
    const uint32_t m = p.material_indices[i];
    if(m>=slot.size())
      continue;
    const bool beside = slot[m]==uint16_t(-1) && !water[m];
    const bool road   = verge[m].slot!=uint16_t(-1); // kept whole: the strip is wider than a row of its triangles
    const bool shares = corner[p.vertex_indices[i*3]] || corner[p.vertex_indices[i*3+1]] || corner[p.vertex_indices[i*3+2]];
    if(beside && !shares && !road)
      continue;
    if(i<p.flags.size()) {
      const auto& f = p.flags[i];
      if(f.is_portal!=0 || f.is_sector!=0 || f.is_lod!=0 || f.is_ghost_occluder!=0)
        continue;
      }
    float pos[3][3] = {}, uv[3][2] = {}, normal[3][3] = {};
    for(size_t k=0; k<3; ++k) {
      const auto& v = mesh.vertices[p.vertex_indices[i*3+k]];
      const auto& f = mesh.features[p.feature_indices[i*3+k]];
      pos[k][0]    = v.x;        pos[k][1]    = v.y;        pos[k][2]    = v.z;
      uv[k][0]     = f.texture.x; uv[k][1]    = f.texture.y;
      normal[k][0] = f.normal.x; normal[k][1] = f.normal.y; normal[k][2] = f.normal.z;
      }
    if(water[m])
      Grass::addWater(*summary,pos);
    else if(beside) {
      if(shares)
        Grass::addBeside(*summary,pos,normal);
      if(road)
        Grass::addVerge(*summary,pos,normal,verge[m].slot,verge[m].reach,verge[m].margin);
      }
    else
      Grass::addGrass(*summary,pos,uv,normal,slot[m]);
    }
  plots.addTo(*summary,id);

  std::lock_guard guard(sync);
  auto& c   = chunks[key(id.x,id.z)];
  c.summary = std::move(summary);
  c.epoch   = ++epoch;
  dirty.store(true);
  if(layers) {
    borderWant.store(epoch);
    wake.notify_one();
    }
  }

void GrassField::dropChunk(ChunkId id) {
  if(!blades && !layers)
    return;
  std::lock_guard guard(sync);
  if(chunks.erase(key(id.x,id.z))!=0) {
    ++epoch;
    dirty.store(true);
    if(layers) {
      borderWant.store(epoch);
      wake.notify_one();
      }
    }
  }

// The worker's: the borders of the chunks that are in. A chunk's pieces are found anew when it or one of the eight
// round it came or went
void GrassField::buildBorder() {
  std::unordered_map<uint64_t,Chunk> snap;
  uint64_t                           want = 0;
  {
  std::lock_guard guard(sync);
  snap = chunks;
  want = epoch;
  }
  std::vector<uint64_t> keys;
  for(auto& i:snap)
    if(i.second.summary!=nullptr && !i.second.summary->grass.empty())
      keys.push_back(i.first);
  std::sort(keys.begin(),keys.end());

  std::vector<Grass::BorderSeg>       all;
  std::unordered_map<uint64_t,Border> next;
  for(uint64_t k:keys) {
    const int32_t x = int32_t(uint32_t(k >> 32)), z = int32_t(uint32_t(k));
    const Grass::Summary* around[9] = {};
    size_t   count = 0;
    uint64_t from  = 0;
    bool     any   = false;
    for(int32_t i=-1; i<=1; ++i)
      for(int32_t j=-1; j<=1; ++j) {
        const auto it = snap.find(key(x+i,z+j));
        from = from*0x9E3779B97F4A7C15ull + (it!=snap.end() ? it->second.epoch : 0);
        if(it==snap.end() || it->second.summary==nullptr)
          continue;
        around[count++] = it->second.summary.get();
        any = any || !it->second.summary->verge.empty();
        }
    if(!any)
      continue;
    Border b;
    if(const auto it = borderOf.find(k); it!=borderOf.end() && it->second.from==from) {
      b = std::move(it->second);
      } else {
      b.from = from;
      try {
        b.segs = Grass::chunkBorder(*snap[k].summary, std::span<const Grass::Summary*const>(around,count));
        }
      catch(const std::exception& e) {
        Log::e("grass: the border of chunk ",x,",",z," failed: ",e.what());
        }
      }
    all.insert(all.end(), b.segs.begin(), b.segs.end());
    next.emplace(k,std::move(b));
    }
  borderOf = std::move(next);
  auto grid = Grass::packBorder(all);

  std::lock_guard guard(sync);
  borderGrid  = std::move(grid);
  borderNew   = true;
  borderBuilt = want;
  }

// The main thread's: the worker's borders into the buffer of the ground's shader
void GrassField::takeBorder() {
  std::vector<uint32_t> grid;
  uint64_t              built = 0;
  {
  std::lock_guard guard(sync);
  if(!borderNew)
    return;
  grid      = std::move(borderGrid);
  built     = borderBuilt;
  borderNew = false;
  }
  if(grid!=borderCpu) {
    if(!borderCpu.empty())
      Resources::recycle(std::move(borderGpu));
    borderGpu = Resources::ssbo(grid.data(), grid.size()*sizeof(uint32_t));
    borderCpu = std::move(grid);
    scene.grassBorder = &borderGpu;
    }
  borderHave.store(built);
  }

void GrassField::run() {
  Workers::setThreadName("Grass placement");
  for(;;) {
    Job  job;
    bool border = false;
    {
    std::unique_lock guard(sync);
    wake.wait(guard,[&] { return stopping || !jobs.empty() || (layers && borderBuilt!=epoch); });
    if(stopping)
      return;
    border = layers && borderBuilt!=epoch;
    }
    if(border) {
      // before any cell: the ground under the blades is on the screen first
      buildBorder();
      continue;
      }
    {
    std::unique_lock guard(sync);
    if(jobs.empty())
      continue;
    // the nearest cell first
    auto at = std::min_element(jobs.begin(),jobs.end(),[](const Job& a, const Job& b) { return a.dist<b.dist; });
    job = std::move(*at);
    jobs.erase(at);
    }

    std::vector<const Grass::Summary*> summaries;
    for(auto& s:job.summaries)
      summaries.push_back(s.get());
    Grass::CellInput in;
    in.cx        = job.cx;
    in.cz        = job.cz;
    in.summaries = summaries;
    in.params    = job.params;
    in.items     = job.items;
    in.rankLimit = rankLimit(job.level);
    // the fine blades' ring: whole in a cell of the first level, the share at its near edge in the next, none beyond
    in.rankLimitFine = job.level==0 ? 256u : (job.level==1 ? uint32_t(std::ceil(256.f*RankShare*FineFull/(LevelFrom[1]*ring.full))) : 0u);
    in.density   = ring.density;
    in.patch     = Grass::forcedPatch();
    in.squares   = !Grass::squaresOff();

    Done d;
    d.cx    = job.cx;
    d.cz    = job.cz;
    d.level = job.level;
    d.epoch = job.epoch;
    try {
      d.blades = Grass::buildCell(in);
      }
    catch(const std::exception& e) {
      Log::e("grass: cell ",job.cx,",",job.cz," failed: ",e.what());
      }
    std::lock_guard guard(sync);
    done.push_back(std::move(d));
    }
  }

void GrassField::release(Cell& c) {
  if(c.count!=0)
    Resources::recycle(std::move(c.gpu));
  c.gpu     = StorageBuffer();
  c.batches.clear();
  c.cpu.clear();
  c.ranks.clear();
  c.count   = 0;
  c.live    = false;
  }

void GrassField::upload(Cell& c, Done&& d) {
  release(c);
  c.live    = true;
  c.level   = d.level;
  c.epoch   = d.epoch;
  c.pending = false;
  c.minY    = d.blades.minY;
  c.maxY    = d.blades.maxY;
  c.count   = uint32_t(d.blades.blades.size());
  c.squareBounds = d.blades.squareBounds;
  c.ranks.clear();
  if(c.count==0)
    return;
  c.batches = std::move(d.blades.batches);
  c.reach   = 80.f;
  for(const auto& b:c.batches)
    if(b.kind==Grass::KindEar)
      c.reach = Grass::EarTop;
  c.ranks.resize(c.count);
  for(size_t i=0; i<c.count; ++i)
    c.ranks[i] = uint8_t(d.blades.blades[i].w[1] >> 24);
  c.gpu     = Resources::ssbo(d.blades.blades.data(), d.blades.blades.size()*sizeof(Grass::Blade));
  if(Grass::wasteOn())
    c.cpu = std::move(d.blades.blades);
  }

void GrassField::update() {
  if(layers)
    takeBorder();
  if(!blades)
    return;
  scene.setGrassRing(Vec4(ring.full, ring.fadeFrom, ring.radius, 1.f + 2.f*float(Grass::partsOff())));
  const Vec3    cam = scene.camPos();
  const int32_t ccx = int32_t(std::floor(cam.x/CellCm)), ccz = int32_t(std::floor(cam.z/CellCm));
  const int32_t r   = int32_t(std::ceil(ring.radius/CellCm));

  // what the worker has made, and the chunks round the camera
  struct Near final {
    bool                                  inWorld = false;
    std::shared_ptr<const Grass::Summary> summary;
    uint64_t                              epoch = 0;
    };
  constexpr int32_t NearR = 3;
  Near          near[2*NearR+1][2*NearR+1];
  const int32_t chx = floorDiv(ccx,ChunkCells), chz = floorDiv(ccz,ChunkCells);
  size_t        queued = 0;
  {
  std::lock_guard guard(sync);
  dirty.store(false);
  for(auto& d:done)
    toUpload.push_back(std::move(d));
  done.clear();
  queued = jobs.size();
  for(int32_t x=-NearR; x<=NearR; ++x)
    for(int32_t z=-NearR; z<=NearR; ++z) {
      auto& n = near[x+NearR][z+NearR];
      const uint64_t k = key(chx+x,chz+z);
      n.inWorld = world.find(k)!=world.end();
      if(const auto it = chunks.find(k); it!=chunks.end()) {
        n.summary = it->second.summary;
        n.epoch   = it->second.epoch;
        }
      }
  if(slotParams.size()!=slots.size()) {
    slotParams.clear();
    for(auto& s:slots)
      slotParams.push_back(s.params);
    }
  }
  // the ground's own texture of a new material: the colour under the roots
  while(slotTex.size()<slotParams.size()) {
    asset::Material m;
    {
    std::lock_guard guard(sync);
    m = slots[slotTex.size()].material;
    }
    // of a ground with its own layers the sward's map as well, laid by the world's x and z (Grass::Params::tile)
    const auto& mat = Resources::loadMaterial(m,true);
    const bool  own = slotParams[slotTex.size()].cover && mat.layers.present();
    slotSward.push_back(own ? mat.layers.sward[0] : nullptr);
    slotTex.push_back(mat.tex);
    }

  // buffers: a budget a frame, the nearest results first
  size_t bytes = 0;
  while(!toUpload.empty() && (bytes==0 || bytes<UploadBytes)) {
    Done d = std::move(toUpload.front());
    toUpload.pop_front();
    const auto it = cells.find(key(d.cx,d.cz));
    if(it==cells.end() || !it->second.pending || it->second.reqLevel!=d.level || it->second.reqEpoch!=d.epoch)
      continue; // nobody waits for it any more
    bytes += d.blades.blades.size()*sizeof(Grass::Blade);
    upload(it->second,std::move(d));
    }

  // cells that left the ring
  for(auto it=cells.begin(); it!=cells.end();) {
    auto& c = it->second;
    const float dx = std::max(0.f, std::max(float(c.cx)*CellCm - cam.x, cam.x - float(c.cx+1)*CellCm));
    const float dz = std::max(0.f, std::max(float(c.cz)*CellCm - cam.z, cam.z - float(c.cz+1)*CellCm));
    c.dist = std::sqrt(dx*dx + dz*dz);
    if(c.dist>ring.radius+CellCm) {
      release(c);
      it = cells.erase(it);
      } else {
      ++it;
      }
    }

  // what the ring needs
  std::vector<Job> ask;
  size_t           waiting = toUpload.size();
  for(int32_t x=ccx-r; x<=ccx+r; ++x)
    for(int32_t z=ccz-r; z<=ccz+r; ++z) {
      const float dx   = std::max(0.f, std::max(float(x)*CellCm - cam.x, cam.x - float(x+1)*CellCm));
      const float dz   = std::max(0.f, std::max(float(z)*CellCm - cam.z, cam.z - float(z+1)*CellCm));
      const float dist = std::sqrt(dx*dx + dz*dz);
      if(dist>ring.radius)
        continue;

      // its chunk and the eight round it
      const int32_t kx = floorDiv(x,ChunkCells) - chx + NearR, kz = floorDiv(z,ChunkCells) - chz + NearR;
      if(kx<1 || kz<1 || kx+1>2*NearR || kz+1>2*NearR)
        continue;
      bool     all = true, any = false;
      uint64_t ep  = 0;
      for(int32_t i=-1; i<=1; ++i)
        for(int32_t j=-1; j<=1; ++j) {
          const auto& n = near[kx+i][kz+j];
          if(n.summary!=nullptr) {
            any = any || !n.summary->empty();
            ep  = std::max(ep,n.epoch);
            }
          else if(n.inWorld)
            all = false;
          }
      const uint64_t k  = key(x,z);
      auto           it = cells.find(k);
      if(!any && it==cells.end())
        continue; // no grass here, and none was
      if(!all) {
        ++waiting;
        continue;
        }

      const uint8_t hi = levelAt(dist-LevelLead), lo = levelAt(dist-LevelSlack);
      if(it!=cells.end()) {
        const auto& c = it->second;
        if(c.live && c.epoch==ep && c.level>=lo && c.level<=hi && !c.pending)
          continue;
        if(c.pending && c.reqEpoch==ep && c.reqLevel>=lo && c.reqLevel<=hi) {
          ++waiting;
          continue;
          }
        }
      ++waiting;
      if(!any) {
        // the grass that was here has gone with its chunk
        release(it->second);
        it->second.pending = false;
        it->second.live    = true;
        it->second.epoch   = ep;
        it->second.level   = hi;
        --waiting;
        continue;
        }
      Job job;
      job.cx    = x;
      job.cz    = z;
      job.level = hi;
      job.epoch = ep;
      job.dist  = dist;
      for(int32_t i=-1; i<=1; ++i)
        for(int32_t j=-1; j<=1; ++j)
          if(const auto& n = near[kx+i][kz+j]; n.summary!=nullptr && (!n.summary->empty() || !n.summary->verge.empty()))
            job.summaries.push_back(n.summary);
      ask.push_back(std::move(job));
      }

  if(!ask.empty() && queued<MaxJobs) {
    std::sort(ask.begin(),ask.end(),[](const Job& a, const Job& b) { return a.dist<b.dist; });
    if(ask.size()>MaxJobs-queued)
      ask.resize(MaxJobs-queued);
    for(auto& job:ask) {
      auto& c = cells[key(job.cx,job.cz)];
      c.cx       = job.cx;
      c.cz       = job.cz;
      c.dist     = job.dist;
      c.pending  = true;
      c.reqLevel = job.level;
      c.reqEpoch = job.epoch;
      job.params = slotParams;
      if(items) {
        const float pad = 100.f;
        items(float(job.cx)*CellCm-pad, float(job.cz)*CellCm-pad, float(job.cx+1)*CellCm+pad, float(job.cz+1)*CellCm+pad, job.items);
        }
      }
    std::lock_guard guard(sync);
    for(auto& job:ask)
      jobs.push_back(std::move(job));
    wake.notify_one();
    }

  last.waiting = waiting;
  }

#include "grassfield.h"

#include <Tempest/Device>
#include <Tempest/Log>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

#include "graphics/dynamic/frustrum.h"
#include "graphics/sceneglobals.h"
#include "graphics/shaders.h"
#include "utils/workers.h"
#include "resources.h"

using namespace Tempest;

namespace {

constexpr float   CellCm     = float(Grass::CellSize/Grass::Unit);
constexpr int32_t ChunkCells = ChunkId::Size/int32_t(Grass::CellSize/Grass::Unit);
static_assert(ChunkId::Size%(Grass::CellSize/Grass::Unit)==0, "a chunk is a whole number of cells");

// the camera's distance at which a cell takes its next level, cm: of Grass::Ring::full; the first is the whole
constexpr float LevelFrom[] = {0.f, 1.2f, 2.f, 3.2f, 4.8f};
constexpr float LevelLead   = 200.f; // cm: more blades are asked for this early
constexpr float LevelSlack  = 600.f; // and fewer this late
constexpr float RankShare   = 1.12f; // grass.vert: a blade stands while its rank is under full/d*1.12
constexpr float KeepBeyond  = CellCm; // a cell out of the ring is kept this far

// links of a blade by the distance of its cell, and their triangles in the index buffer
constexpr float   SegmentsTo[] = {1000.f, 2500.f, 4500.f};
constexpr uint8_t MaxSegments  = 4;

constexpr size_t  UploadBytes  = 512*1024; // a frame; one cell at least
constexpr size_t  RankCounts   = 257;
constexpr float   RootsBeyond  = 100.f;    // cm: a root lies no nearer to the camera than its cell less this
constexpr size_t  MaxJobs      = 6;

struct Push final {
  Vec4     origin;
  float    wind         = 1.f;
  float    translucency = 0.f;
  uint32_t first        = 0;
  uint32_t segments     = 0;
  };

int32_t floorDiv(int32_t a, int32_t b) {
  const int32_t q = a/b;
  return (a%b!=0 && ((a<0)!=(b<0))) ? q-1 : q;
  }

size_t indexOffset(uint8_t segments) {
  // the buffer holds the blades of 4, 3, 2 and 1 links one after another
  size_t at = 0;
  for(uint8_t s=MaxSegments; s>segments; --s)
    at += size_t(2*s-1)*3;
  return at;
  }

}

GrassField::GrassField(SceneGlobals& scene, std::vector<ChunkId> ids)
  :scene(scene) {
  const auto sw = Grass::enabled();
  ring   = Grass::ring(sw.mode);
  blades = sw.blades;
  for(auto& i:ids)
    world.insert(key(i.x,i.z));

  std::vector<uint16_t> index;
  for(uint8_t s=MaxSegments; s>=1; --s) {
    for(uint16_t k=0; k+1<s; ++k) {
      const uint16_t v = uint16_t(2*k);
      const uint16_t quad[] = {v, uint16_t(v+1), uint16_t(v+2), uint16_t(v+1), uint16_t(v+3), uint16_t(v+2)};
      index.insert(index.end(), std::begin(quad), std::end(quad));
      }
    const uint16_t v = uint16_t(2*(s-1));
    const uint16_t tip[] = {v, uint16_t(v+1), uint16_t(v+2)};
    index.insert(index.end(), std::begin(tip), std::end(tip));
    }
  ibo = Resources::ibo(index.data(), index.size());

  scene.setGrassRing(Vec4(ring.full, ring.fadeFrom, ring.radius, blades ? 1.f : 0.5f));
  if(blades)
    worker = std::thread([this] { run(); });
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
  const auto params = Resources::grassParams(name);
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

void GrassField::addChunk(ChunkId id, const asset::WorldMesh& mesh) {
  if(!blades)
    return;
  auto  summary = std::make_shared<Grass::Summary>();
  auto& p       = mesh.polygons;

  std::vector<uint16_t> slot(mesh.materials.size(), uint16_t(-1));
  std::vector<bool>     water(mesh.materials.size(), false);
  {
  std::lock_guard guard(sync);
  for(size_t i=0; i<mesh.materials.size(); ++i) {
    water[i] = mesh.materials[i].group==asset::MaterialGroup::WATER;
    if(!water[i])
      slot[i] = slotOf(mesh.materials[i]);
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
    if(beside && !corner[p.vertex_indices[i*3]] && !corner[p.vertex_indices[i*3+1]] && !corner[p.vertex_indices[i*3+2]])
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
    else if(beside)
      Grass::addBeside(*summary,pos,normal);
    else
      Grass::addGrass(*summary,pos,uv,normal,slot[m]);
    }

  std::lock_guard guard(sync);
  auto& c   = chunks[key(id.x,id.z)];
  c.summary = std::move(summary);
  c.epoch   = ++epoch;
  dirty.store(true);
  }

void GrassField::dropChunk(ChunkId id) {
  if(!blades)
    return;
  std::lock_guard guard(sync);
  if(chunks.erase(key(id.x,id.z))!=0) {
    ++epoch;
    dirty.store(true);
    }
  }

void GrassField::run() {
  Workers::setThreadName("Grass placement");
  for(;;) {
    Job job;
    {
    std::unique_lock guard(sync);
    wake.wait(guard,[&] { return stopping || !jobs.empty(); });
    if(stopping)
      return;
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
    in.density   = ring.density;

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
  c.below.clear();
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
  c.below.clear();
  if(c.count==0)
    return;
  c.batches = std::move(d.blades.batches);
  c.below.assign(c.batches.size()*RankCounts, 0);
  for(size_t i=0; i<c.batches.size(); ++i) {
    const auto& b     = c.batches[i];
    uint32_t*   below = &c.below[i*RankCounts];
    for(uint32_t k=0; k<b.count; ++k)
      ++below[(d.blades.blades[b.first+k].w[1] >> 24) + 1];
    for(size_t r=1; r<RankCounts; ++r)
      below[r] += below[r-1];
    }
  c.gpu     = Resources::ssbo(d.blades.blades.data(), d.blades.blades.size()*sizeof(Grass::Blade));
  }

void GrassField::update() {
  if(!blades)
    return;
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
    slotTex.push_back(Resources::loadMaterial(m,true).tex);
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
    if(c.dist>ring.radius+KeepBeyond) {
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
          if(const auto& n = near[kx+i][kz+j]; n.summary!=nullptr && !n.summary->empty())
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

void GrassField::draw(Encoder<CommandBuffer>& cmd) {
  Stats st;
  st.waiting = last.waiting;
  if(!blades || cells.empty()) {
    last = st;
    return;
    }
  const auto& fr  = scene.frustrum[SceneGlobals::V_Main];
  const float pad = float(Grass::CellMargin/Grass::Unit) + 80.f;
  bool        bound = false;
  for(auto& i:cells) {
    auto& c = i.second;
    if(!c.live || c.count==0)
      continue;
    ++st.cells;
    st.stored += c.count;
    st.bytes  += size_t(c.count)*sizeof(Grass::Blade);
    if(c.dist>ring.radius)
      continue;
    const Vec3 min(float(c.cx)*CellCm-pad,   float(c.minY)/float(Grass::Unit)-10.f, float(c.cz)*CellCm-pad);
    const Vec3 max(float(c.cx+1)*CellCm+pad, float(c.maxY)/float(Grass::Unit)+80.f, float(c.cz+1)*CellCm+pad);
    if(fr.testBbox(min,max)==Frustrum::T_Invisible)
      continue;

    uint8_t segments = MaxSegments;
    for(float to:SegmentsTo)
      if(c.dist>=to)
        --segments;
    // the ranks that stand at the nearest of the cell (grass.vert)
    const auto ranks = size_t(std::min(256.f, std::ceil(256.f*RankShare*ring.full/std::max(c.dist-RootsBeyond,1.f))));

    if(!bound) {
      cmd.setPipeline(Shaders::inst().grass);
      cmd.setBinding(0, scene.uboGlobal[SceneGlobals::V_Main]);
      bound = true;
      }
    cmd.setBinding(1, c.gpu);
    for(size_t bi=0; bi<c.batches.size(); ++bi) {
      const auto&    b     = c.batches[bi];
      const uint32_t count = c.below[bi*RankCounts+ranks];
      if(b.slot>=slotTex.size() || slotTex[b.slot]==nullptr || count==0)
        continue;
      st.drawn     += count;
      st.triangles += size_t(count)*size_t(2*segments-1);
      const auto& p = slotParams[b.slot];
      Push push;
      push.origin       = Vec4(float(c.cx)*CellCm - float(Grass::CellMargin/Grass::Unit), float(c.minY)/float(Grass::Unit),
                               float(c.cz)*CellCm - float(Grass::CellMargin/Grass::Unit), 0.f);
      push.wind         = p.wind;
      push.translucency = p.translucency;
      push.first        = b.first;
      push.segments     = segments;
      cmd.setBinding(2, *slotTex[b.slot], Sampler::trillinear());
      cmd.setPushData(push);
      cmd.draw(nullptr, ibo, indexOffset(segments), size_t(2*segments-1)*3, 0, count);
      }
    }
  last = st;
  }

#include "grassfield.h"

#include <Tempest/Device>

#include <chrono>
#include <cmath>

#include "graphics/dynamic/frustrum.h"
#include "graphics/grass/grasscull.h"
#include "graphics/sceneglobals.h"
#include "graphics/shaders.h"
#include "resources.h"

using namespace Tempest;

namespace {

using Clock = std::chrono::steady_clock;

constexpr float   SquareCm     = float(Grass::SquareSize/Grass::Unit);
constexpr float   RootsBeyond  = 100.f;    // cm: a root lies no nearer to the camera than its cell less this
constexpr uint32_t WasteEvery  = 30;       // frames between two counts of Grass::Waste

// The fine blades of the short sward (Grass::Params::fine): a ring of their own, cm. All of them to FineFull, a
// share FineFull/d from there, into the ground between FineFade and FineRadius; two links near, one further
constexpr float   FineRadius   = 2000.f;

struct Push final {
  Vec4     origin;
  float    wind         = 1.f;
  float    translucency = 0.f;
  uint32_t table        = 0;     // of the draw's spans in the frame's table (grass.vert)
  uint32_t segments     = 0;
  Vec4     shape;                  // the width at the root and per step, cm; the ring's full and fade, 0 the scene's
  float    swardK       = 0.f;     // repeats of the sward's map in a cm, by the world's x and z; 0 without own layers
  uint32_t pack         = 1;
  uint32_t kind         = 0;     // Grass::KindEar: stalks with an ear (grass_ear.glsl)
  };

uint64_t elapsedNs(Clock::time_point start) {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-start).count());
  }

}

void GrassField::prepare(uint8_t fId) {
  Stats st;
  st.pack = Grass::packSize();
  st.waiting = last.waiting;
  st.cull = last.cull;
  // the count of the blades submitted for nothing walks every drawn blade: one frame of WasteEvery, kept between
  const bool waste = Grass::wasteOn() && (wasteFrame++ % WasteEvery)==0;
  if(!waste)
    st.waste = last.waste;
  st.drawUs = last.drawUs;
  const bool timeDraw = Grass::wasteOn() && !waste;
  const auto t0 = timeDraw ? Clock::now() : Clock::time_point();
  draws.clear();
  drawIbo = nullptr;
  drawIndirect = nullptr;
  prepareNs = 0;
  if(!blades || cells.empty()) {
    last = st;
    scene.setGrassDrawn(false);
    return;
    }
  const auto& fr  = scene.frustrum[SceneGlobals::V_Main];
  const bool  squares = !Grass::squaresOff();
  const Vec3  cam     = scene.camPos();

  // A cell is drawn by its squares (reports/engine/2026-11-17-grass-cost): each is tested against the view and has
  // the ranks of its own nearest point, so the blades out of the view and those that by their distance have gone
  // into the ground are not submitted with the rest of the cell. The squares of a ground and a kind go in one draw:
  // its blades are packed in the order of the squares, and the span table maps them to the cell buffer (grass.vert).
  // The links of a blade are the cell's, as before the squares.
  draws.reserve(128);
  spanCpu.clear();
  Grass::PackSpans spans(spanCpu, st.pack);
  for(auto& i:cells) {
    auto& c = i.second;
    if(!c.live || c.count==0)
      continue;
    ++st.cells;
    st.stored += c.count;
    st.bytes  += size_t(c.count)*sizeof(Grass::Blade);
    if(c.dist>ring.radius)
      continue;
    const float pad = float(Grass::CellMargin/Grass::Unit) + c.reach;
    const Vec3 min(float(c.cx)*CellCm-pad,   float(c.minY)/float(Grass::Unit)-10.f, float(c.cz)*CellCm-pad);
    const Vec3 max(float(c.cx+1)*CellCm+pad, float(c.maxY)/float(Grass::Unit)+c.reach, float(c.cz+1)*CellCm+pad);
    const auto cellView = fr.testBbox(min,max);
    if(cellView==Frustrum::T_Invisible)
      continue;

    const uint8_t segments = Grass::broadLinks(c.dist);

    float   dist   = c.dist;
    int32_t tested = -1;
    bool    inView = true;
    Draw    dr;
    auto flush = [&]() {
      if(dr.count==0)
        return;
      dr.instances = spans.finish();
      st.instances += dr.instances;
      st.drawn     += dr.count;
      st.fine      += dr.fine ? dr.count : 0;
      st.triangles += size_t(dr.count)*size_t(2*dr.links-1);
      draws.push_back(dr);
      dr.count = 0;
      };
    for(size_t bi=0; bi<c.batches.size(); ++bi) {
      const auto& b    = c.batches[bi];
      const bool  fine = b.kind==1;
      if(dr.count!=0 && (dr.slot!=b.slot || dr.fine!=fine))
        flush();
      if(squares && int32_t(b.square)!=tested) {
        tested = int32_t(b.square);
        const float sx = float(c.cx)*CellCm + float(b.square/Grass::CellSquares)*SquareCm;
        const float sz = float(c.cz)*CellCm + float(b.square%Grass::CellSquares)*SquareCm;
        const float dx = std::max(0.f, std::max(sx - cam.x, cam.x - (sx+SquareCm)));
        const float dz = std::max(0.f, std::max(sz - cam.z, cam.z - (sz+SquareCm)));
        dist   = std::sqrt(dx*dx + dz*dz);
        const auto& bounds = c.squareBounds[b.square];
        const float y0 = float(bounds.minY)/float(Grass::Unit)-10.f;
        const float y1 = float(bounds.maxY)/float(Grass::Unit)+c.reach;
        inView = dist<=ring.radius && (cellView==Frustrum::T_Full ||
                 fr.testBbox(Vec3(sx-pad,y0,sz-pad), Vec3(sx+SquareCm+pad,y1,sz+SquareCm+pad))!=Frustrum::T_Invisible);
        }
      if(!inView || (fine && dist>=FineRadius))
        continue;
      // the ranks that stand at the nearest of the square (grass.vert)
      const size_t   rk    = size_t(std::min(256.f, std::ceil(256.f*RankShare*(fine ? FineFull : ring.full)/std::max(dist-RootsBeyond,1.f))));
      const uint8_t  links = fine ? uint8_t(c.dist<FineFull ? 2 : 1) : segments;
      const uint8_t* r0    = c.ranks.data()+b.first;
      const auto     count = uint32_t(std::lower_bound(r0, r0+b.count, rk, [](uint8_t r, size_t lim) { return size_t(r)<lim; }) - r0);
      if(b.slot>=slotTex.size() || slotTex[b.slot]==nullptr || count==0)
        continue;
      if(waste && !c.cpu.empty()) {
        Grass::WasteRule rule;
        rule.full        = fine ? FineFull   : ring.full;
        rule.fadeFrom    = fine ? Grass::FineFade : ring.fadeFrom;
        rule.radius      = fine ? FineRadius : ring.radius;
        rule.cellRadius  = ring.radius;
        rule.rankShare   = RankShare;
        rule.rootsBeyond = RootsBeyond;
        rule.fine        = fine;
        rule.heightStep  = Grass::heightStep(b.kind);
        Grass::WasteCell wc;
        wc.origin = Vec3(float(c.cx)*CellCm - float(Grass::CellMargin/Grass::Unit), float(c.minY)/float(Grass::Unit),
                         float(c.cz)*CellCm - float(Grass::CellMargin/Grass::Unit));
        wc.x0     = float(c.cx)*CellCm;
        wc.z0     = float(c.cz)*CellCm;
        wc.size   = CellCm;
        wc.minY   = min.y;
        wc.maxY   = max.y;
        wc.pad    = pad;
        wc.dist   = c.dist;
        Grass::countWaste(st.waste, fr, cam, rule, wc, std::span<const Grass::Blade>(c.cpu.data()+b.first, count), links);
        }
      if(dr.count==0) {
        dr.cell  = &c;
        dr.slot  = b.slot;
        dr.fine  = fine;
        dr.links = links;
        dr.table = uint32_t(spanCpu.size());
        spans.reset();
        }
      spans.add(b.first, count);
      dr.count += count;
      }
    flush();
    }

  if(!draws.empty()) {
    // The draws keep their original offsets into spanCpu. Stable ordering keeps each cell's batches in order.
    Grass::orderDraws(draws);

    auto&        device = Resources::device();
    const auto   packAt = std::find(Grass::PackSizes.begin(), Grass::PackSizes.end(), st.pack);
    auto&        ibo    = packBuffers[size_t(packAt-Grass::PackSizes.begin())];
    if(ibo.isEmpty())
      ibo = device.ibo(Grass::packIndices(st.pack));
    if(!Grass::gpuCullEnabled()) {
      auto&        buf    = spanGpu[fId];
      const size_t bytes  = spanCpu.size()*sizeof(uint32_t);
      if(buf.byteSize()<bytes) {
        Resources::recycle(std::move(buf));
        buf = device.ssbo(BufferHeap::Upload, Uninitialized, std::max<size_t>(bytes+bytes/2, 4096));
        }
      buf.update(spanCpu.data(), 0, bytes);
      }

    drawIbo = &ibo;
    drawIndirect = uploadIndirect(draws, fId, st.pack);
    }
  st.draws = draws.size();
  last = st;
  scene.setGrassDrawn(st.drawn>0);
  if(timeDraw)
    prepareNs = elapsedNs(t0);
  }

void GrassField::draw(Encoder<CommandBuffer>& cmd, uint8_t fId, bool historyOwner) {
  const auto t0 = prepareNs!=0 ? Clock::now() : Clock::time_point();
  if(!draws.empty()) {
    const auto& ibo = *drawIbo;
    const auto  pack = last.pack;
    const auto& shaders = Shaders::inst();
    const bool cull = Grass::gpuCullEnabled();
    const auto& grass = cull ? shaders.grassCulled : shaders.grass;
    cmd.setPipeline(grass[historyOwner]);
    cmd.setBinding(0, scene.uboGlobal[SceneGlobals::V_Main]);
    cmd.setBinding(4, cull ? cullGpu[fId].compact : spanGpu[fId]);
    if(cull)
      cmd.setBinding(5, cullGpu[fId].windows);
    const Cell* cellBound = nullptr;
    size_t      slotBound = size_t(-1);
    for(size_t i=0; i<draws.size(); ++i) {
      const auto& dr = draws[i];
      const auto& c = *dr.cell;
      const auto& p = slotParams[dr.slot];
      Push push;
      push.origin       = Grass::bladeOrigin(c.cx, c.cz, c.minY);
      push.wind         = p.wind;
      push.translucency = p.translucency;
      push.table        = cull ? uint32_t(i) : dr.table;
      push.segments     = dr.links;
      push.pack         = pack;
      push.kind         = p.kind;
      push.shape        = Grass::bladeShape(dr.fine, FineFull);
      const auto* sward = slotSward[dr.slot];
      push.swardK       = (sward!=nullptr && p.tile>0.f) ? 1.f/p.tile : 0.f;
      if(cellBound!=&c) {
        cmd.setBinding(1, c.gpu);
        cellBound = &c;
        }
      if(slotBound!=dr.slot) {
        cmd.setBinding(2, *slotTex[dr.slot], Sampler::trillinear());
        cmd.setBinding(3, sward!=nullptr ? *sward : *slotTex[dr.slot], Sampler::trillinear());
        slotBound = dr.slot;
        }
      cmd.setPushData(push);
      if(drawIndirect!=nullptr)
        cmd.drawIndexedIndirect(ibo, *drawIndirect, i*sizeof(DrawIndexedIndirectCommand));
      else
        cmd.draw(nullptr, ibo, Grass::packIndexOffset(dr.links, pack), Grass::packIndexCount(dr.links, pack), 0, dr.instances);
      }
    }
  if(prepareNs!=0) {
    // Only the first draw contributes to the diagnostic, even when history verification replays this list.
    drawNs += prepareNs + elapsedNs(t0);
    prepareNs = 0;
    if(++drawFrames==WasteEvery-1) {
      last.drawUs = size_t(drawNs/(1000u*drawFrames));
      drawNs      = 0;
      drawFrames  = 0;
      }
    }
  }

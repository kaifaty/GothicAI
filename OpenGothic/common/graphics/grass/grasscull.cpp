#include "grasscull.h"
#include "grassfield.h"

#include <Tempest/Device>
#include <Tempest/Log>

#include <cstdlib>
#include <limits>

#include "graphics/sceneglobals.h"
#include "graphics/shaders.h"
#include "resources.h"
#include "shader.h"

using namespace Tempest;

namespace {

struct CullDraw {
  Vec4     origin, shape;
  uint32_t cell, kind;
  uint32_t pad[2] = {};
  };
static_assert(sizeof(CullDraw)==48);

struct CullBlock {
  uint32_t draw, first, count, pad = 0;
  };
static_assert(sizeof(CullBlock)==16);

void reserveBuffer(StorageBuffer& buffer, BufferHeap heap, size_t bytes) {
  if(buffer.byteSize()>=bytes)
    return;
  Resources::recycle(std::move(buffer));
  buffer = Resources::device().ssbo(heap, Uninitialized, std::max<size_t>(bytes+bytes/2, 4096));
  }

}

bool Grass::gpuCullEnabled() {
  static const bool enabled = [] {
    const char* env = std::getenv("OPENGOTHIC_GRASS_GPU_CULL");
    if(env==nullptr || env[0]!='1' || env[1]!='\0')
      return false;
    if(!Resources::device().properties().descriptors.nonUniformIndexing) {
      Log::w("Grass GPU culling requires storage buffer descriptor arrays");
      return false;
      }
    return true;
    }();
  return enabled;
  }

void Shaders::compileGrassShaders(bool historySupported) {
  // Blades are visible on both sides and write the main view's depth.
  auto& device = Resources::device();
  RenderState state;
  state.setCullFaceMode(RenderState::CullMode::NoCull);
  state.setZTestMode(RenderState::ZTestMode::LEqual);
  state.setZWriteEnabled(true);

  const auto vertex = GothicShader::get("grass.vert.sprv");
  const auto frag = GothicShader::get("grass.frag.sprv");
  const auto vs = device.shader(vertex.data, vertex.len);
  const auto fs = device.shader(frag.data, frag.len);
  Shader fsHistory;
  grass[0] = device.pipeline(Triangles, state, vs, fs);
  if(historySupported) {
    const auto history = GothicShader::get("grass_hist.frag.sprv");
    fsHistory = device.shader(history.data, history.len);
    grass[1] = device.pipeline(Triangles, state, vs, fsHistory);
    }
  grassContactMask = computeShader("grass_contact_mask.comp.sprv");
  if(Grass::gpuCullEnabled()) {
    const auto compact = GothicShader::get("grass_cull.vert.sprv");
    const auto vsCompact = device.shader(compact.data, compact.len);
    grassCulled[0] = device.pipeline(Triangles, state, vsCompact, fs);
    if(historySupported)
      grassCulled[1] = device.pipeline(Triangles, state, vsCompact, fsHistory);
    grassCull = computeShader(Grass::wasteOn() ? "grass_cull_stats.comp.sprv" : "grass_cull.comp.sprv");
    grassCullFinish = computeShader("grass_cull_finish.comp.sprv");
    }
  }

void GrassField::cull(Encoder<CommandBuffer>& cmd, uint8_t fId, const Texture2d& hiZ, float depthRoundoff) {
  if(!Grass::gpuCullEnabled())
    return;

  auto& device = Resources::device();
  auto& frame = cullGpu[fId];
  const bool stats = Grass::wasteOn();
  // The fence for this frame slot has already been waited on. The count and its candidate total belong together.
  if(frame.candidates!=0) {
    uint32_t rejected[3] = {};
    device.readBytes(frame.stats, rejected, sizeof(rejected));
    last.cull = {frame.candidates, rejected[0], rejected[1], rejected[2]};
    frame.candidates = 0;
    }
  if(draws.empty())
    return;

  const auto& shaders = Shaders::inst();
  const uint32_t pack = last.pack;
  const uint32_t spanStride = pack==1 ? 2u : 3u;
  const uint32_t lanes = uint32_t(shaders.grassCull.workGroupSize().x);
  std::vector<const StorageBuffer*> cellBuffers;
  std::vector<CullDraw> drawData;
  std::vector<CullBlock> blocks;
  std::vector<std::array<uint32_t,2>> windows;
  drawData.reserve(draws.size());
  windows.reserve(draws.size());
  uint32_t windowStart = 0;
  for(const auto& draw : draws) {
    const auto& c = *draw.cell;
    if(cellBuffers.empty() || cellBuffers.back()!=&c.gpu)
      cellBuffers.push_back(&c.gpu);
    const auto drawIndex = uint32_t(drawData.size());
    drawData.push_back({Grass::bladeOrigin(c.cx, c.cz, c.minY), Grass::bladeShape(draw.fine, FineFull),
                        uint32_t(cellBuffers.size()-1), slotParams[draw.slot].kind});
    windows.push_back({windowStart, 0});
    windowStart += draw.count;

    // Each workgroup reads a contiguous slice of an existing span. In the packed table, the final
    // instance in a span may contain padding, which is excluded using the span's exact blade end.
    uint32_t prev = 0;
    for(uint32_t s=0; s<uint32_t(Grass::CellSquares*Grass::CellSquares); ++s) {
      const auto* span = spanCpu.data()+draw.table+s*spanStride;
      const uint32_t end = span[0];
      if(end==std::numeric_limits<uint32_t>::max())
        break;
      const uint32_t first = span[1]+prev*pack;
      const uint32_t count = pack==1 ? end-prev : span[2]-first;
      for(uint32_t i=0; i<count; i+=lanes)
        blocks.push_back({drawIndex, first+i, std::min(lanes, count-i)});
      prev = end;
      }
    }

  frame.cells = device.descriptors(cellBuffers);
  reserveBuffer(frame.draws,   BufferHeap::Upload, drawData.size()*sizeof(CullDraw));
  reserveBuffer(frame.blocks,  BufferHeap::Upload, blocks.size()*sizeof(CullBlock));
  reserveBuffer(frame.windows, BufferHeap::Upload, windows.size()*sizeof(windows.front()));
  reserveBuffer(frame.compact, BufferHeap::Device, size_t(windowStart)*sizeof(uint32_t));
  frame.draws.update(drawData.data(), 0, drawData.size()*sizeof(CullDraw));
  frame.blocks.update(blocks.data(), 0, blocks.size()*sizeof(CullBlock));
  frame.windows.update(windows.data(), 0, windows.size()*sizeof(windows.front()));
  if(stats) {
    const uint32_t zero[3] = {};
    if(frame.stats.isEmpty())
      frame.stats = device.ssbo(BufferHeap::Readback, Uninitialized, sizeof(zero));
    frame.stats.update(zero, 0, sizeof(zero));
    frame.candidates = windowStart;
    }

  struct CullPush {
    uint32_t blocks;
    float    znear;
    uint32_t rawDepth;
    float    depthRoundoff;
    } push = {uint32_t(blocks.size()), scene.znear, uint32_t(hiZ.format()==TextureFormat::R32F), depthRoundoff};
  cmd.setFramebuffer({});
  cmd.setDebugMarker("GrassCull");
  cmd.setPipeline(shaders.grassCull);
  cmd.setBinding(0, scene.uboGlobal[SceneGlobals::V_Main]);
  cmd.setBinding(1, frame.cells);
  cmd.setBinding(2, frame.draws);
  cmd.setBinding(3, frame.blocks);
  cmd.setBinding(4, hiZ, Sampler::nearest());
  cmd.setBinding(5, frame.compact);
  cmd.setBinding(6, frame.windows);
  if(stats)
    cmd.setBinding(7, frame.stats);
  cmd.setPushData(push);
  const size_t groupsX = std::min<size_t>(blocks.size(), 1024);
  cmd.dispatch(groupsX, (blocks.size()+groupsX-1)/groupsX);

  const uint32_t finish[] = {uint32_t(draws.size()), pack};
  cmd.setPipeline(shaders.grassCullFinish);
  cmd.setBinding(0, frame.windows);
  cmd.setBinding(1, *drawIndirect);
  cmd.setPushData(finish);
  cmd.dispatchThreads(draws.size());
  }

#include "drawcommands.h"

#include <Tempest/Log>
#include <algorithm>
#include <cstdlib>

#include "drawclusters.h"
#include "frameprofile.h"
#include "settings.h"
#include "shaders.h"
#include "treeboughrender.h"
#include "treeboughdraw.h"
#include "visualobjects.h"

using namespace Tempest;

bool DrawCommands::treeHistoryEnabled() {
  if(!::Settings::exists())
    return false;
  static const bool enabled = [] {
    const char* env = std::getenv("OPENGOTHIC_HIZ_HISTORY");
    const bool configured = env!=nullptr && *env!='\0' ? std::string_view(env)!="0" :
                                                      ::Settings::getI("INTERNAL", "hizHistory")!=0;
    const bool on = configured && Shaders::isHiZHistorySupported();
    Log::i("render: tree HiZ history = ", on ? "1" : "0");
    return on;
    }();
  return enabled;
  }

uint32_t DrawCommands::historyClusterCount() const {
  return uint32_t(clusters.size());
  }

bool DrawCommands::prepareHistory(Encoder<CommandBuffer>& enc, const Texture2d& winners, uint8_t fId) {
  if(cmd.empty() || clusters.size()==0 || clusters.size()>=0xffffffu || maxPayload==0)
    return false;
  auto& device = Resources::device();
  if(history.dirty) {
    // The map follows capacities, not visible counts. Each block covers at most 128 consecutive
    // meshlets of one draw, so independent workgroups can compact without changing raster order.
    std::vector<HistoryCommand> commands(cmd.size());
    uint32_t blockCount = 0;
    for(size_t id=0; id<cmd.size(); ++id) {
      const auto& cx = cmd[id];
      if(cx.pMain==nullptr || (cx.alpha!=Material::Solid && cx.alpha!=Material::AlphaTest) || cx.maxPayload==0)
        continue;
      if(cx.pHistoryGBuffer==nullptr || cx.pHistoryDepth==nullptr || cx.pHistoryVerify==nullptr)
        return false;
      const uint32_t count = (cx.maxPayload+127)/128;
      commands[id] = {blockCount, count, 1u, 0u};
      blockCount += count;
      }
    if(blockCount==0)
      return false;
    // Each active command determines every block's owner and local offset.
    if(commands!=history.uploadedCommands) {
      struct Block { uint32_t command, offset; };
      std::vector<Block> blocks;
      blocks.reserve(blockCount);
      for(size_t id=0; id<commands.size(); ++id)
        for(uint32_t i=0; i<commands[id].blocks; ++i)
          blocks.push_back({uint32_t(id), i*128});
      auto commandBuffer = device.ssbo(commands);
      auto blockBuffer = device.ssbo(blocks);
      Resources::recycle(std::move(history.commands));
      Resources::recycle(std::move(history.blocks));
      history.commands = std::move(commandBuffer);
      history.blocks = std::move(blockBuffer);
      history.blockCount = blockCount;
      history.uploadedCommands = std::move(commands);
      }
    const auto reserve = [&](StorageBuffer& buffer, size_t bytes) {
      if(buffer.byteSize()>=bytes)
        return;
      Resources::recycle(std::move(buffer));
      buffer = device.ssbo(Uninitialized, bytes);
      };
    reserve(history.compactScratch, size_t(blockCount)*5*sizeof(uint32_t));
    reserve(history.view.visClusters, maxPayload*4*sizeof(uint32_t));
    reserve(history.view.indirectCmd, cmd.size()*sizeof(IndirectCmd));
    history.dirty = false;
    }
  const size_t slots = (clusters.size()+1023) & ~size_t(1023);
  if(history.status.byteSize()<slots*sizeof(uint32_t)) {
    Resources::recycle(std::move(history.status));
    history.status = device.ssbo(Uninitialized, slots*sizeof(uint32_t));
    for(auto& count : history.counts) {
      Resources::recycle(std::move(count));
      count = device.ssbo(Uninitialized, slots*2*sizeof(uint32_t));
      }
    history.previousCount = 0;
    }
  if(history.stats.isEmpty())
    history.stats = device.ssbo(Uninitialized, 5*sizeof(uint32_t));
  if(FrameProfile::enabled() && history.written[fId]) {
    uint32_t values[5] = {};
    device.readBytes(history.read[fId], values, sizeof(values));
    FrameProfile::add(FrameProfile::Amount::HiZHistoryEarlyClusters, values[0]);
    FrameProfile::add(FrameProfile::Amount::HiZHistoryEarlyMeshlets, values[1]);
    FrameProfile::add(FrameProfile::Amount::HiZHistoryTestedClusters, values[2]);
    FrameProfile::add(FrameProfile::Amount::HiZHistoryRejectedClusters, values[3]);
    FrameProfile::add(FrameProfile::Amount::HiZHistoryRejectedMeshlets, values[4]);
    history.written[fId] = false;
    }
  sampleHistory(enc, winners, 2);
  return true;
  }

void DrawCommands::classifyHistory(Encoder<CommandBuffer>& enc, const Texture2d& depth,
                                   float depthRoundoff, bool late, bool reset) {
  static const uint32_t threshold = uint32_t(std::max(1, ::Settings::getI("INTERNAL", "hizHistoryMinWins")));
  struct Push {
    uint32_t clusterCount;
    float    znear;
    float    depthRoundoff;
    uint32_t phase;
    uint32_t threshold, statsEnabled, previousCount, pad;
    } push = {historyClusterCount(), scene.znear, depthRoundoff, uint32_t(late),
              threshold, uint32_t(FrameProfile::enabled()), reset ? 0 : history.previousCount, 0};
  enc.setFramebuffer({});
  enc.setDebugMarker(late ? "HiZ-history-retest" : "HiZ-history-seeds");
  enc.setBinding(0, scene.uboGlobal[SceneGlobals::V_Main]);
  enc.setBinding(1, clusters.ssbo());
  enc.setBinding(2, owner.instanceSsbo());
  enc.setBinding(3, buckets.ssbo());
  enc.setBinding(4, history.counts[history.readIndex]);
  enc.setBinding(5, depth, Sampler::nearest());
  enc.setBinding(6, history.status);
  enc.setBinding(7, history.stats);
  enc.setBinding(10, buckets.treeHiZBounds());
  enc.setPushData(push);
  enc.setPipeline(boughHistoryToken!=0 ? boughs->historyClassify() : Shaders::inst().hizHistoryClassify);
  enc.dispatchThreads(push.clusterCount);
  }

void DrawCommands::compactHistory(Encoder<CommandBuffer>& enc, bool early) {
  auto& main = views[SceneGlobals::V_Main];
  struct Push { uint32_t phase, blockCount, commandCount, early; };
  Push push = {0, history.blockCount, uint32_t(cmd.size()), uint32_t(early)};
  enc.setFramebuffer({});
  enc.setDebugMarker(early ? "HiZ-history-pack-seeds" : "HiZ-history-pack-final");
  enc.setBinding(0, main.visClusters);
  enc.setBinding(1, main.indirectCmd);
  enc.setBinding(2, history.status);
  enc.setBinding(3, history.view.visClusters);
  enc.setBinding(4, history.view.indirectCmd);
  enc.setBinding(5, history.commands);
  enc.setBinding(6, history.blocks);
  enc.setBinding(7, history.compactScratch);
  enc.setPipeline(Shaders::inst().hizHistoryCompact);
  for(push.phase=0; push.phase<3; ++push.phase) {
    enc.setPushData(push);
    if(push.phase==1)
      enc.dispatch(push.commandCount);
    else
      enc.dispatch(push.blockCount);
    }
  }

void DrawCommands::sampleHistory(Encoder<CommandBuffer>& enc, const Texture2d& winners, uint32_t phase) {
  const uint32_t count = historyClusterCount();
  const uint32_t push[] = {count, phase};
  enc.setFramebuffer({});
  enc.setBinding(0, winners, Sampler::nearest());
  enc.setBinding(1, history.counts[1-history.readIndex]);
  enc.setBinding(2, clusters.ssbo());
  enc.setBinding(3, history.status);
  enc.setBinding(4, history.stats);
  enc.setPushData(push);
  enc.setPipeline(Shaders::inst().hizHistorySample);
  if(phase==1)
    enc.dispatchThreads((size_t(winners.w())+3)/4, (size_t(winners.h())+3)/4);
  else
    enc.dispatch((std::max(count,5u)+63)/64);
  }

void DrawCommands::finishHistory(Encoder<CommandBuffer>& enc, const Texture2d& winners, uint8_t fId) {
  if(boughExpanded && TreeBough::coverageEnabled())
    boughs->countCoverage(enc,winners);
  enc.setDebugMarker("HiZ-history-owners");
  sampleHistory(enc, winners, 0);
  sampleHistory(enc, winners, 1);
  history.readIndex = 1-history.readIndex;
  history.previousCount = historyClusterCount();
  if(!FrameProfile::enabled())
    return;

  if(history.read[fId].isEmpty())
    history.read[fId] = Resources::device().ssbo(BufferHeap::Readback, Uninitialized, 5*sizeof(uint32_t));
  enc.setBinding(0, history.read[fId]);
  enc.setBinding(1, history.stats);
  enc.setPipeline(Shaders::inst().copyBuf);
  enc.dispatchThreads(5);
  history.written[fId] = true;
  }

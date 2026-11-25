#include "hizhistoryverify.h"

#include <Tempest/Log>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <utility>
#include <vector>

#include "resources.h"
#include "shader.h"

using namespace Tempest;

void HiZHistoryVerify::compare(Encoder<CommandBuffer>& cmd, const ComputePipeline& pipeline,
                               const BenchProbe::HistoryVerifyRequest& request,
                               const Texture2d& earlyDepth, const Texture2d& earlyOwners,
                               const Texture2d& referenceDepth, const Texture2d& referenceOwners,
                               const Texture2d& enabledDepth, const Texture2d& enabledOwners,
                               const StorageBuffer& flags, uint32_t clusters, bool reset) {
  if(request.number!=recorded || recorded>=PairCount || clusters==0) {
    Log::e("HiZ history verification: unexpected pair ", request.number, " or empty cluster list");
    return;
    }

  auto& device = Resources::device();
  if(results.isEmpty()) {
    std::array<Counters,PairCount> rows = {};
    for(auto& row : rows)
      row.back() = std::numeric_limits<uint32_t>::max();
    results = device.ssbo(BufferHeap::Readback, rows.data(), sizeof(rows));
    }
  if(seen.byteSize()<size_t(clusters)*sizeof(uint32_t)) {
    Resources::recycle(std::move(seen));
    seen = device.ssbo(std::vector<uint32_t>(clusters, 0));
    }

  samples[recorded++] = {std::string(request.segment), request.frame, request.elapsedMs,
                         uint32_t(referenceDepth.w()), uint32_t(referenceDepth.h()), reset};
  const uint32_t push[] = {request.number, clusters};
  cmd.setFramebuffer({});
  cmd.setDebugMarker("HiZ-history-verify");
  cmd.setBinding(0, earlyDepth, Sampler::nearest());
  cmd.setBinding(1, earlyOwners, Sampler::nearest());
  cmd.setBinding(2, referenceDepth, Sampler::nearest());
  cmd.setBinding(3, referenceOwners, Sampler::nearest());
  cmd.setBinding(4, enabledDepth, Sampler::nearest());
  cmd.setBinding(5, enabledOwners, Sampler::nearest());
  cmd.setBinding(6, flags);
  cmd.setBinding(7, results);
  cmd.setBinding(8, seen);
  cmd.setPushData(push);
  cmd.setPipeline(pipeline);
  cmd.dispatchThreads(referenceDepth.size());
  }

bool HiZHistoryVerify::finish() {
  const auto* outDir = std::getenv("OPENGOTHIC_BENCH_OUT");
  if(recorded==0 || outDir==nullptr) {
    Log::e("HiZ history verification: no rendered pairs or missing OPENGOTHIC_BENCH_OUT");
    return false;
    }
  std::array<Counters,PairCount> rows = {};
  auto& device = Resources::device();
  device.waitIdle(); // A single read after the last submitted pair.
  device.readBytes(results, rows.data(), sizeof(rows));

  std::ofstream out(std::string(outDir) + "/tree-hidden-3b.csv");
  out << "segment,frame,elapsed_ms,reset,pixels,removed_winning_pixels,removed_winning_clusters,owner_diff,depth_diff,"
         "early_same_owner,early_depth_diff,early_closer_same_owner,canopy_same_owner,canopy_depth_diff,canopy_closer,"
         "early_closer_any_owner,early_clusters,tested_clusters,rejected_clusters,first_bad_x,first_bad_y\n";
  const std::pair<std::string_view,uint32_t> expected[] = {
    {"inside", 16}, {"far-100", 16}, {"far-300", 16}, {"turn", 32},
    {"cut-before", 8}, {"cut-after", 8}, {"pass", 120},
    {"crest-1", 8}, {"crest-2", 8}, {"crest-3", 8}, {"crest-4", 8}
    };
  size_t segment = 0;
  uint32_t frame = 0;
  uint64_t early = 0, crowns = 0, rejected = 0;
  bool pass = recorded==PairCount;
  for(uint32_t i=0; i<recorded; ++i) {
    const auto& sample = samples[i];
    const auto& row = rows[i];
    const auto firstBad = row.back();
    out << sample.segment << ',' << sample.frame << ',' << sample.elapsedMs << ',' << sample.reset;
    for(size_t field=0; field+1<row.size(); ++field)
      out << ',' << row[field];
    if(firstBad==std::numeric_limits<uint32_t>::max())
      out << ",,\n";
    else
      out << ',' << firstBad%sample.width << ',' << firstBad/sample.width << '\n';
    pass &= row[0]==uint64_t(sample.width)*sample.height && firstBad==std::numeric_limits<uint32_t>::max();
    pass &= segment<std::size(expected) && sample.segment==expected[segment].first && sample.frame==frame;
    if(segment<std::size(expected) && ++frame==expected[segment].second) {
      ++segment;
      frame = 0;
      }
    if(sample.segment=="cut-after" && sample.frame==0)
      pass &= sample.reset && row[12]==0 && row[13]==0 && row[14]==0;
    if(sample.segment=="pass" && sample.frame==119)
      pass &= sample.elapsedMs>=9000;
    early += row[5];
    crowns += row[8];
    rejected += row[14];
    }
  out.close();
  pass &= bool(out) && segment==std::size(expected) && early>0 && crowns>0 && rejected>0;
  Log::i("HiZ history verification: ", recorded, "/", PairCount, " pairs, ", early, " early winners, ",
         crowns, " canopy winners, ", rejected, " rejected clusters, ", pass ? "passed" : "failed");
  return pass;
  }

void BoughHiZVerify::compare(Encoder<CommandBuffer>& cmd, const BenchProbe::HistoryVerifyRequest& request,
                            const Texture2d& referenceDepth, const Texture2d& referenceOwners,
                            const Texture2d& enabledDepth, const Texture2d& enabledOwners, bool reset,
                            bool optimized) {
  if(request.number!=recorded || recorded>=MaxPairs) {
    failed = true;
    Log::e("Bough HiZ verification: unexpected pair ", request.number);
    return;
    }

  auto& device = Resources::device();
  if(pipeline.isEmpty()) {
    const auto shader = GothicShader::get("tree_bough_hiz_verify.comp.sprv");
    pipeline = device.pipeline(device.shader(shader.data, shader.len));
    }
  if(results.isEmpty()) {
    std::array<Counters, MaxPairs> rows = {};
    for(auto& row : rows)
      row.back() = std::numeric_limits<uint32_t>::max();
    results = device.ssbo(BufferHeap::Readback, rows.data(), sizeof(rows));
    }

  samples[recorded++] = {std::string(request.segment), request.frame, request.elapsedMs,
                         uint32_t(referenceDepth.w()), uint32_t(referenceDepth.h()), reset, optimized};
  cmd.setFramebuffer({});
  cmd.setDebugMarker("Bough-HiZ-verify");
  cmd.setBinding(0, referenceDepth, Sampler::nearest());
  cmd.setBinding(1, referenceOwners, Sampler::nearest());
  cmd.setBinding(2, enabledDepth, Sampler::nearest());
  cmd.setBinding(3, enabledOwners, Sampler::nearest());
  cmd.setBinding(4, results);
  cmd.setPushData(request.number);
  cmd.setPipeline(pipeline);
  cmd.dispatchThreads(referenceDepth.size());
  }

bool BoughHiZVerify::finish() {
  const auto* outDir = std::getenv("OPENGOTHIC_BENCH_OUT");
  if(recorded==0 || outDir==nullptr) {
    Log::e("Bough HiZ verification: no rendered pairs or missing OPENGOTHIC_BENCH_OUT");
    return false;
    }
  std::array<Counters, MaxPairs> rows = {};
  auto& device = Resources::device();
  device.waitIdle();
  device.readBytes(results, rows.data(), sizeof(rows));
  results = {};

  std::ofstream out(std::string(outDir) + "/bough-hiz-verify.csv");
  out << "segment,frame,elapsed_ms,reset,optimized,width,height,pixels,owner_diff,depth_diff,coverage_diff,"
         "reference_bough_pixels,enabled_bough_pixels,first_bad_x,first_bad_y\n";
  bool pass = !failed;
  uint64_t comparedPixels = 0, boughPixels = 0;
  uint32_t optimizedPairs = 0;
  for(uint32_t i=0; i<recorded; ++i) {
    const auto& sample = samples[i];
    const auto& row = rows[i];
    const auto firstBad = row.back();
    out << sample.segment << ',' << sample.frame << ',' << sample.elapsedMs << ',' << sample.reset << ','
        << sample.optimized << ',' << sample.width << ',' << sample.height;
    for(size_t field=0; field+1<row.size(); ++field)
      out << ',' << row[field];
    if(firstBad==std::numeric_limits<uint32_t>::max())
      out << ",,\n";
    else
      out << ',' << firstBad%sample.width << ',' << firstBad/sample.width << '\n';
    pass &= row[0]==uint64_t(sample.width)*sample.height && firstBad==std::numeric_limits<uint32_t>::max();
    pass &= !sample.reset || !sample.optimized;
    optimizedPairs += sample.optimized;
    comparedPixels += row[0];
    boughPixels += row[4];
    }
  out.close();
  pass &= bool(out) && boughPixels>0 && optimizedPairs>0;
  Log::i("Bough HiZ verification: ", recorded, " pairs (", optimizedPairs, " optimized), ", comparedPixels, " pixels, ",
         boughPixels, " compact foliage winners, ", pass ? "passed" : "failed");
  recorded = 0;
  failed = false;
  return pass;
  }

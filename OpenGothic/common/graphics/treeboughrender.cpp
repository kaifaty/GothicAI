#include "treeboughrender.h"

#include <Tempest/Log>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <stdexcept>

#include "frameprofile.h"
#include "graphics/drawclusters.h"
#include "graphics/mesh/submesh/staticmesh.h"
#include "graphics/shaders.h"
#include "graphics/treeboughasset.h"
#include "graphics/treeboughdraw.h"
#include "graphics/treeboughgroups.h"
#include "graphics/visualobjects.h"
#include "shader.h"

using namespace Tempest;

namespace {

ComputePipeline compute(std::string_view name) {
  auto& device = Resources::device();
  const auto shader = GothicShader::get(name);
  return device.pipeline(device.shader(shader.data,shader.len));
  }

Vec3 column(const Matrix4x4& m, size_t i) {
  return Vec3(m[i][0], m[i][1], m[i][2]);
  }

TreeBough::Projection project(const Matrix4x4& m, const SceneGlobals& scene) {
  const Vec3 x = column(m,0), y = column(m,1), z = column(m,2);
  const auto dot = Vec3::dotProduct;
  const float xy = std::abs(dot(x,y)), yz = std::abs(dot(y,z)), zx = std::abs(dot(z,x));
  const float scale = std::sqrt(std::max({dot(x,x)+xy+zx, dot(y,y)+yz+xy, dot(z,z)+zx+yz}));
  const Vec3 forward(scene.view[0][2], scene.view[1][2], scene.view[2][2]);
  return {Vec3(dot(forward,x), dot(forward,y), dot(forward,z)),
          dot(forward,column(m,3))+scene.view[3][2],
          0.5f*float(scene.screenRes().y)*std::abs(scene.verticalProjection())*scale, scene.znear};
  }

void putVec(std::vector<uint32_t>& words, Vec3 v) {
  words.push_back(std::bit_cast<uint32_t>(v.x));
  words.push_back(std::bit_cast<uint32_t>(v.y));
  words.push_back(std::bit_cast<uint32_t>(v.z));
  }

void demandProxyTextures(const TreeBough::Asset& asset, bool add) {
  for(size_t i=2; i<asset.pages.size(); ++i)
    Resources::textureDemand(asset.pages[i].material,Resources::TextureCategory::Static,add);
  }

}

TreeBoughRender::TreeBoughRender(DrawCommands& owner)
  : owner(owner), expandPipeline(compute("tree_bough_expand.comp.sprv")),
    copyPipeline(compute("tree_bough_buffer.comp.sprv")), cpu(TreeBough::cpuEnabled()) {}

TreeBoughRender::~TreeBoughRender() {
  for(const auto& [mesh,asset]:assets)
    demandProxyTextures(*mesh->treeBough,false);
  }

uint32_t TreeBoughRender::command(const Material& material, uint16_t bucket, uint32_t unit) {
  const bool bindless = Shaders::options().doBindless && !material.hasFrameAnimation();
  const uint32_t bucketId = bindless ? uint32_t(-1) : bucket;
  const auto pso = Shaders::inst().materialPipeline(material, DrawCommands::Static,
                                                   Shaders::T_Main, bindless, Shaders::Bough::Cards);
  for(uint32_t i=0; i<commands.size(); ++i)
    if(commands[i].draw.pMain==pso && commands[i].draw.bucketId==bucketId && commands[i].unit==unit)
      return i;
  if(commands.size()>=65536)
    throw std::runtime_error("Too many compact foliage draw commands");
  auto& dest = commands.emplace_back();
  dest.draw.pMain = pso;
  if(DrawCommands::treeHistoryEnabled())
    dest.draw.pHistoryGBuffer = Shaders::inst().materialPipeline(material, DrawCommands::Static,
                                                                  Shaders::T_HistoryGBuffer, bindless, Shaders::Bough::Cards);
  dest.draw.alpha = material.alpha;
  dest.draw.type = DrawCommands::Static;
  dest.draw.bucketId = bucketId;
  dest.unit = unit;
  return uint32_t(commands.size()-1);
  }

void TreeBoughRender::registerAsset(const StaticMesh& mesh, Asset& asset) {
  const auto& source = *mesh.treeBough;
  asset.buckets.resize(source.pages.size());
  std::map<std::pair<uint32_t,uint32_t>,uint32_t> perClass;
  asset.rangeCommands.resize(source.ranges.size(), uint32_t(-1));
  for(size_t i=0; i<source.ranges.size(); ++i) {
    const auto& range = source.ranges[i];
    if(range.indexCount==0)
      continue;
    auto& bucket = asset.buckets[range.primitive];
    const auto& page = source.pages[range.primitive];
    auto pos = perClass.lower_bound({range.primitive,0});
    if(pos==perClass.end() || pos->first.first!=range.primitive)
      bucket = owner.buckets.alloc(page.material,mesh,&page);
    const uint32_t count = Shaders::options().doMeshShading ? TreeBough::groupSize(range) :
      *std::lower_bound(TreeBough::indexClasses.begin(), TreeBough::indexClasses.end(), range.indexCount);
    auto [at, newClass] = perClass.try_emplace({range.primitive,count},0);
    if(newClass)
      at->second = command(page.material, bucket.toInt(), count);
    asset.rangeCommands[i] = at->second | (uint32_t(bucket.toInt()) << 16u);
    }

  // A parent's band can overlap only the coarse cut below it. Otherwise the child cuts can peak independently.
  struct CutCapacity {
    uint32_t coarse = 0, peak = 0;
    };
  std::vector<CutCapacity> branch(source.nodes.size());
  for(const auto& [key,id]:perClass) {
    std::fill(branch.begin(), branch.end(), CutCapacity{});
    uint32_t capacity = 0;
    for(size_t i=source.nodes.size(); i-- > 0;) {
      const auto& node = source.nodes[i];
      auto [coarse,peak] = branch[i];
      if(node.highPx>0.f || node.subtreeEnd==i+1u) {
        const uint32_t own = source.ranges[i].primitive==key.first && (asset.rangeCommands[i] & 0xffffu)==id;
        peak = std::max(peak,coarse+own);
        coarse = own;
        }
      if(node.parent==TreeBough::NoNode) {
        capacity += peak;
        } else {
        branch[node.parent].coarse += coarse;
        branch[node.parent].peak += peak;
        }
      }
    asset.capacity.emplace_back(id,capacity);
    }
  }

void TreeBoughRender::add(const StaticMesh& mesh, uint32_t cluster, uint16_t main, uint16_t impostor,
                          uint32_t first, uint32_t count) {
  auto [asset, inserted] = assets.try_emplace(&mesh);
  if(inserted) {
    registerAsset(mesh,asset->second);
    demandProxyTextures(*mesh.treeBough,true);
    }
  ++asset->second.refs;
  for(auto [id,capacity]:asset->second.capacity)
    commands[id].draw.maxPayload += capacity;

  entries.emplace(cluster,Entry{&mesh,uint32_t(main) | (uint32_t(impostor)<<16u),first,count,0});
  if(cpu)
    references.try_emplace(cluster);
  dirty = true;
  }

void TreeBoughRender::remove(uint32_t cluster) {
  const auto found = entries.find(cluster);
  if(found==entries.end())
    return;
  const auto& e = found->second;
  auto asset = assets.find(e.mesh);
  for(auto [id,capacity]:asset->second.capacity)
    commands[id].draw.maxPayload -= capacity;
  if(--asset->second.refs==0) {
    demandProxyTextures(*e.mesh->treeBough,false);
    assets.erase(asset);
    }
  references.erase(cluster);
  entries.erase(found);
  dirty = true;
  }

void TreeBoughRender::setTransform(uint32_t cluster, const Matrix4x4& transform) {
  const auto ref = references.find(cluster);
  if(ref!=references.end())
    ref->second = transform;
  }

void TreeBoughRender::copy(Encoder<CommandBuffer>& enc, const StorageBuffer& source, StorageBuffer& destination,
                           uint32_t offset, uint32_t count, uint32_t sourceCount) {
  const uint32_t push[] = {offset,count,sourceCount};
  enc.setFramebuffer({});
  enc.setBinding(0, source);
  enc.setBinding(1, destination);
  enc.setPushData(push);
  enc.setPipeline(copyPipeline);
  enc.dispatchThreads(count);
  }

void TreeBoughRender::uploadData() {
  auto& device = Resources::device();
  const uint32_t mapCount = entries.rbegin()->first+1u;
  std::vector<uint32_t> words(4u+mapCount);
  words[0] = mapCount;
  const uint32_t entryBase = uint32_t(words.size());
  words[1] = entryBase;
  words.resize(words.size()+entries.size()*8u);
  for(auto& [mesh,asset]:assets) {
    const auto& source = *mesh->treeBough;
    const auto& box = mesh->bbox.bbox;
    const auto& bound = mesh->drawn.bbox;
    const uint32_t base = uint32_t(words.size());
    const uint32_t roots = base+20u;
    const uint32_t nodes = roots+uint32_t(source.roots.size());
    const uint32_t ranges = nodes+16u*uint32_t(source.nodes.size());
    float ratio = 0.f;
    for(const auto& n:source.nodes)
      if(n.highPx>0.f)
        ratio = std::max(ratio,n.highPx/n.diameter);
    words.insert(words.end(),{roots,uint32_t(source.roots.size()),nodes,uint32_t(source.nodes.size()),
                              ranges,source.sourceCards,source.sourceTriangles,std::bit_cast<uint32_t>(ratio)});
    putVec(words,(box[0]+box[1])*0.5f);
    words.push_back(std::bit_cast<uint32_t>(box[1].y-box[0].y));
    putVec(words,bound[0]);
    putVec(words,bound[1]);
    words.push_back(std::bit_cast<uint32_t>(source.cullPadding));
    words.resize(roots);
    words.insert(words.end(),source.roots.begin(),source.roots.end());
    words.resize(ranges);
    std::memcpy(words.data()+nodes,source.nodes.data(),source.nodes.size()*sizeof(TreeBough::Node));
    const bool packed = Shaders::options().doMeshShading;
    for(size_t i=0; i<source.ranges.size(); ++i) {
      const auto& r = source.ranges[i];
      // tree_bough_mesh.glsl: a mesh workgroup lays out its nodes without reading their index headers
      const uint32_t first = packed ? r.firstIndex | (r.vertexCount << TreeBough::FirstIndexBits) : r.firstIndex;
      words.insert(words.end(),{first,r.indexCount,asset.rangeCommands[i],r.cardCount});
      }
    asset.offset = base;
    }
  uint32_t entry = entryBase;
  uint32_t stateOffset = StatsCount;
  referenceSize = 0;
  for(auto& [cluster,v]:entries) {
    words[4u+cluster] = entry;
    v.referenceOffset = referenceSize;
    if(cpu)
      referenceSize += 1u + 2u*uint32_t(v.mesh->treeBough->nodes.size());
    const uint32_t record[] = {cluster,v.commands,v.first,v.count,assets.at(v.mesh).offset,
                               stateOffset++,v.referenceOffset,0u};
    std::copy_n(record,8,words.data()+entry);
    entry += 8u;
    }
  Resources::recycle(std::move(data));
  data = device.ssbo(words);

  Resources::recycle(std::move(state));
  state = device.ssbo(std::vector<uint32_t>(stateOffset));

  std::vector<DrawCommands::IndirectCmd> draws;
  draws.reserve(commands.size());
  size_t capacity = 0;
  for(auto& c:commands) {
    const bool mesh = c.draw.isMeshShader();
    c.draw.firstPayload = uint32_t(capacity);
    // The last mesh workgroup of a command is padded to whole c.unit nodes.
    capacity += mesh ? TreeBough::groupSlots(c.draw.maxPayload,c.unit) : c.draw.maxPayload;
    draws.push_back({c.unit,0,mesh ? 1u : 0u,mesh ? 1u : 0u,c.draw.firstPayload});
    }
  Resources::recycle(std::move(indirect));
  indirect = device.ssbo(draws);
  const size_t bytes = capacity*4u*sizeof(uint32_t);
  if(payload.byteSize()<bytes || payload.byteSize()>bytes*2u) {
    Resources::recycle(std::move(payload));
    payload = device.ssbo(Uninitialized,bytes);
    }
  }

void TreeBoughRender::uploadReference() {
  auto& device = Resources::device();
  std::vector<uint32_t> words(referenceSize,uint32_t(-1));
  std::vector<TreeBough::Selected> selected;
  for(const auto& [cluster,entry]:entries) {
    const auto& c = owner.clusters[cluster];
    if(c.meshletCount==0 || c.commandId!=uint16_t(entry.commands))
      continue;
    const auto& transform = references.at(cluster);
    const auto& asset = *entry.mesh->treeBough;
    TreeBough::select(asset,project(transform,owner.scene),selected);
    auto* out = words.data()+entry.referenceOffset;
    out[0] = 0;
    for(const auto& s:selected) {
      const uint32_t at = 1u + s.node*2u;
      out[at] = s.transition;
      out[at+1u] = std::bit_cast<uint32_t>(s.weight);
      if(s.transition!=0u || asset.nodes[s.node].subtreeEnd!=s.node+1u)
        out[0] = 1;
      }
    }
  Resources::recycle(std::move(referenceData));
  referenceData = device.ssbo(words);
  }

void TreeBoughRender::commit() {
  if(entries.empty())
    return;
  if(dirty) {
    uploadData();
    dirty = false;
    }
  if(cpu)
    uploadReference();
  }

const ComputePipeline& TreeBoughRender::cull(Encoder<CommandBuffer>& enc, bool hiZ, bool probe, bool history,
                                            bool ordered, uint32_t& token) {
  const uint32_t index = hiZ ? 0u : probe ? 1u : history ? 2u : 3u;
  auto& pipeline = cullPipelines[index][ordered];
  if(pipeline.isEmpty()) {
    static const char* names[4][2] = {
      {"visibility_bough_cr.comp.sprv","visibility_bough_cr_ordered.comp.sprv"},
      {"visibility_bough_probe.comp.sprv","visibility_bough_probe_ordered.comp.sprv"},
      {"visibility_bough_history.comp.sprv","visibility_bough_history_ordered.comp.sprv"},
      {"visibility_bough.comp.sprv","visibility_bough_ordered.comp.sprv"},
      };
    pipeline = compute(names[index][ordered]);
    }
  enc.setBinding(13,data);
  if(!hiZ)
    enc.setBinding(14,state);
  if(!hiZ) {
    epoch += 2u;
    if(epoch==0u)
      epoch = 2u;
    token = epoch;
    }
  return pipeline;
  }

const ComputePipeline& TreeBoughRender::historyClassify() {
  if(historyPipeline.isEmpty())
    historyPipeline = compute("hiz_history_bough_classify.comp.sprv");
  return historyPipeline;
  }

void TreeBoughRender::expand(Encoder<CommandBuffer>& enc, uint32_t token, bool lateHiZ) {
  auto& view = owner.views[SceneGlobals::V_Main];
  const auto& scene = owner.scene;
  uint32_t flags = uint32_t(cpu);
  flags |= FrameProfile::enabled() ? 2u : 0u;
  flags |= scene.isTreeOcclusionProbe() ? 4u : 0u;
  flags |= lateHiZ ? 8u : 0u;
  expandedToken = token;
  expandedFlags = flags;
  struct Push { uint32_t token, flags; float nearZ; uint32_t stage; };
  enc.setBinding(0,scene.uboGlobal[SceneGlobals::V_Main]);
  enc.setBinding(1,view.visClusters);
  enc.setBinding(2,owner.owner.instanceSsbo());
  enc.setBinding(3,data);
  enc.setBinding(4,view.indirectCmd);
  enc.setBinding(5,owner.clusters.ssbo());
  enc.setBinding(6,state);
  enc.setBinding(7,cpu ? referenceData : data);
  enc.setBinding(8,indirect);
  enc.setBinding(9,payload);
  enc.setBinding(10,lateHiZ ? owner.history.status : data);
  enc.setPipeline(expandPipeline);
  enc.setPushData(Push{token,flags,scene.znear,1});
  enc.dispatchThreads(std::max<size_t>(StatsCount,commands.size()));
  enc.setPushData(Push{token,flags,scene.znear,0});
  enc.dispatch(entries.size());
  if(Shaders::options().doMeshShading) {
    enc.setPushData(Push{token,flags,scene.znear,2});
    enc.dispatchThreads(commands.size());
    }
  }

void TreeBoughRender::draw(Encoder<CommandBuffer>& enc, DrawCommands::HistoryDraw history) {
  for(size_t i=0; i<commands.size(); ++i) {
    const auto& c = commands[i].draw;
    const auto pso = history==DrawCommands::HistoryDraw::Off ? c.pMain : c.pHistoryGBuffer;
    if(c.maxPayload==0 || pso==nullptr)
      continue;
    owner.setBindings(enc,c,SceneGlobals::V_Main);
    enc.setBinding(DrawCommands::L_Payload,payload);
    const uint32_t push[] = {c.firstPayload,c.isMeshShader() ? commands[i].unit : c.maxPayload};
    enc.setPushData(push);
    enc.setPipeline(*pso);
    const size_t offset = i*sizeof(DrawCommands::IndirectCmd);
    if(c.isMeshShader())
      enc.dispatchMeshIndirect(indirect,offset+sizeof(uint32_t));
    else
      enc.drawIndirect(indirect,offset);
    }
  }

void TreeBoughRender::readStats(Encoder<CommandBuffer>& enc, uint8_t frame, bool expanded) {
  auto& device = Resources::device();
  constexpr FrameProfile::Amount names[] = {FrameProfile::Amount::TreeBoughNodes,FrameProfile::Amount::TreeBoughSourceCards,
    FrameProfile::Amount::TreeBoughParentCards,FrameProfile::Amount::TreeBoughTriangles,FrameProfile::Amount::TreeBoughActiveBands,
    FrameProfile::Amount::TreeBoughVersionPairs,FrameProfile::Amount::TreeBoughBandTriangles,FrameProfile::Amount::TreeBoughHeightPx};
  static_assert(std::size(names)==StatsCount);
  uint32_t values[std::size(names)] = {};
  if(readPending[frame]) {
    device.readBytes(stats[frame],values,sizeof(values));
    for(size_t i=0; i<std::size(names); ++i)
      FrameProfile::add(names[i],values[i]);
    readPending[frame] = false;
    }
  if(!expanded)
    return;
  if(stats[frame].isEmpty())
    stats[frame] = device.ssbo(BufferHeap::Readback,Uninitialized,sizeof(values));
  copy(enc,state,stats[frame],0,uint32_t(std::size(names)),uint32_t(std::size(names)));
  readPending[frame] = true;
  }

uint32_t TreeBoughRender::copyCommands(Encoder<CommandBuffer>& enc, StorageBuffer& destination) {
  const auto& main = owner.views[SceneGlobals::V_Main];
  const auto& filtered = owner.history.view;
  const StorageBuffer* buffers[] = {&main.indirectCmd, &main.visClusters, &indirect, &payload,
                                    &filtered.indirectCmd, &filtered.visClusters};
  const auto count = (expandedFlags & 8u)!=0u ? std::size(buffers) : std::size(buffers)-2;
  size_t bytes = 0;
  for(size_t i=0; i<count; ++i)
    bytes += buffers[i]->byteSize();
  destination = Resources::device().ssbo(BufferHeap::Readback, Uninitialized, bytes);
  uint32_t offset = 0;
  for(size_t i=0; i<count; ++i) {
    const auto words = uint32_t(buffers[i]->byteSize()/sizeof(uint32_t));
    if(words>0)
      copy(enc,*buffers[i],destination,offset,words,words);
    offset += words;
    }
  return offset;
  }

void TreeBoughRender::readDiagnostic(DiagnosticRead& slot) {
  if(slot.stamp==0)
    return;
  auto& device = Resources::device();
  uint32_t values[DiagnosticCount+StatsCount] = {};
  device.readBytes(slot.counts,values,sizeof(values));
  Log::i("bough_hiz_diagnostic stamp_ns=", slot.stamp, " compact_nodes=", values[0], " compact_triangles=", values[1],
         " hidden_nodes=", values[2], " hidden_triangles=", values[3], " offscreen_nodes=", values[4],
         " offscreen_triangles=", values[5], " submitted_triangles=", values[6],
         " stock_triangles=", slot.stockCounts ? int64_t(values[DiagnosticCount+3]) : -1);
  if(slot.commandWords>0) {
    std::vector<uint32_t> before(slot.commandWords), after(slot.commandWords);
    device.readBytes(slot.before,before.data(),slot.before.byteSize());
    device.readBytes(slot.after,after.data(),slot.after.byteSize());
    size_t different = 0;
    for(size_t i=0; i<before.size(); ++i)
      different += before[i]!=after[i];
    Log::i("bough_hiz_commands stamp_ns=", slot.stamp, " compared_words=", slot.commandWords,
           " different_words=", different);
    slot.before = {};
    slot.after = {};
    slot.commandWords = 0;
    }
  slot.stamp = 0;
  }

bool TreeBoughRender::diagnose(Encoder<CommandBuffer>& enc, uint8_t frame, const Texture2d& depth,
                               const Matrix4x4& viewProject, bool verify, uint64_t stamp) {
  auto& slot = diagnosticRead[frame];
  readDiagnostic(slot); // the frame slot's fence has completed
  if(stamp==0 || !owner.boughExpanded)
    return false;
  auto& device = Resources::device();
  if(diagnosticPipeline.isEmpty()) {
    diagnosticPipeline = compute("tree_bough_hiz_diagnostic.comp.sprv");
    diagnosticCounts = device.ssbo(Uninitialized, DiagnosticCount*sizeof(uint32_t));
    }
  if(slot.counts.isEmpty())
    slot.counts = device.ssbo(BufferHeap::Readback, Uninitialized, (DiagnosticCount+StatsCount)*sizeof(uint32_t));
  if(verify)
    slot.commandWords = copyCommands(enc,slot.before);

  const auto& scene = owner.scene;
  struct Push { uint32_t token, flags; float nearZ; uint32_t stage; Matrix4x4 previousViewProject; };
  Push push = {expandedToken, expandedFlags | 16u, scene.znear, 1, viewProject};
  enc.setFramebuffer({});
  enc.setDebugMarker("Bough-HiZ-diagnostic");
  enc.setBinding(0,scene.uboGlobal[SceneGlobals::V_Main]);
  enc.setBinding(2,owner.owner.instanceSsbo());
  enc.setBinding(3,data);
  enc.setBinding(5,owner.clusters.ssbo());
  enc.setBinding(6,state);
  enc.setBinding(7,cpu ? referenceData : data);
  enc.setBinding(10,(expandedFlags & 8u)!=0u ? owner.history.status : data);
  enc.setBinding(11,depth,Sampler::nearest());
  enc.setBinding(12,diagnosticCounts);
  enc.setPipeline(diagnosticPipeline);
  enc.setPushData(push);
  enc.dispatch(1);
  push.stage = 0;
  enc.setPushData(push);
  enc.dispatch(entries.size());

  if(verify)
    copyCommands(enc,slot.after);
  copy(enc,diagnosticCounts,slot.counts,0,DiagnosticCount,DiagnosticCount);
  slot.stockCounts = (expandedFlags & 2u)!=0u;
  if(slot.stockCounts)
    copy(enc,state,slot.counts,DiagnosticCount,StatsCount,StatsCount);
  slot.stamp = stamp;
  return true;
  }

void DrawCommands::addBough(const StaticMesh& mesh, uint32_t cluster, uint16_t main, uint16_t impostor,
                           uint32_t first, uint32_t count) {
  if(!boughs)
    boughs = std::make_unique<TreeBoughRender>(*this);
  boughs->add(mesh,cluster,main,impostor,first,count);
  }

void DrawCommands::removeBough(uint32_t cluster) {
  if(boughs)
    boughs->remove(cluster);
  }

void DrawCommands::setBoughTransform(uint32_t cluster, const Matrix4x4& transform) {
  if(boughs)
    boughs->setTransform(cluster,transform);
  }

void DrawCommands::finishBoughVisibility(Encoder<CommandBuffer>& enc, uint8_t fId, bool late) {
  if(boughHistoryToken==0u)
    return;
  enc.setDebugMarker("Bough-cut");
  boughs->expand(enc,boughHistoryToken,late);
  boughHistoryToken = 0;
  boughExpanded = true;
  if(FrameProfile::enabled())
    boughs->readStats(enc,fId,true);
  }

bool DrawCommands::diagnoseBoughVisibility(Encoder<CommandBuffer>& enc, uint8_t fId, const Texture2d& depth,
                                           const Matrix4x4& viewProject, bool verify, uint64_t stamp) {
  return boughs && boughs->diagnose(enc,fId,depth,viewProject,verify,stamp);
  }

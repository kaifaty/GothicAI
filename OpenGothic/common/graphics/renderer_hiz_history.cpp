#include "renderer.h"

#include <Tempest/Log>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "frameprofile.h"
#include "renderer_internal.h"
#include "treeboughrender.h"

using namespace Tempest;

void Renderer::seedBoughHiZReference(Encoder<CommandBuffer>& cmd, WorldView& view, bool reset) {
  auto& reference = usesZBuffer(treeHistory.referenceDepth, zBufferFormat, uint32_t(zbuffer.w()), uint32_t(zbuffer.h()));
  std::swap(reference, zbuffer);
  drawHiZ(cmd, view);
  if(!reset) {
    cmd.setFramebuffer({}, {zbuffer, Preserve, Preserve});
    auto& commands = view.treeHistoryCommands();
    constexpr auto draw = DrawCommands::HistoryDraw::Early;
    commands.drawCommon(cmd, SceneGlobals::V_Main, Material::Solid, draw);
    commands.drawCommon(cmd, SceneGlobals::V_Main, Material::AlphaTest, draw);
    }
  // Reset frames expand the boughs before any HiZ dispatch closes this pass.
  cmd.setFramebuffer({});
  std::swap(reference, zbuffer);
  }

void Renderer::drawBoughHiZReference(Encoder<CommandBuffer>& cmd, uint8_t fId, WorldView& view,
                                    DrawCommands::HistoryDraw mode) {
  auto& owners = usesAttachment(treeHistory.referenceOwners, TextureFormat::R32U, zbuffer.size());
  std::swap(treeHistory.referenceDepth, zbuffer);
  drawGBuffer(cmd, fId, view, mode, &owners);
  std::swap(treeHistory.referenceDepth, zbuffer);
  }

void Renderer::compareBoughHiZ(Encoder<CommandBuffer>& cmd, const BenchProbe::HistoryVerifyRequest& request,
                               bool reset, bool optimized) {
  treeHistory.boughVerify.compare(cmd, request, textureCast<const Texture2d&>(treeHistory.referenceDepth),
                                  textureCast<const Texture2d&>(treeHistory.referenceOwners),
                                  textureCast<const Texture2d&>(zbuffer),
                                  textureCast<const Texture2d&>(treeHistory.winners), reset, optimized);
  }

void Renderer::readBoughHiZDepth(uint8_t fId) {
  auto& diag = boughHiZDiag;
  if(diag.proofStamp==0 || diag.proofSlot!=fId)
    return;

  // drawScene is called after the fence for this frame slot has completed.
  const size_t pixelBytes = diag.proofPixelBytes;
  const size_t depthBytes = diag.proofPixels*pixelBytes;
  const size_t stride = (depthBytes+3u)&~size_t(3u);
  std::vector<std::byte> bytes(stride+depthBytes);
  Resources::device().readBytes(diag.proofDepth, bytes.data(), bytes.size());
  size_t different = 0;
  for(size_t i=0; i<depthBytes; i+=pixelBytes)
    different += std::memcmp(bytes.data()+i, bytes.data()+stride+i, pixelBytes)!=0;
  Log::i("bough_hiz_depth stamp_ns=", diag.proofStamp, " compared_pixels=", diag.proofPixels,
         " different_pixels=", different);
  diag.proofStamp = 0;
  Resources::recycle(std::move(diag.proofDepth));
  Resources::recycle(std::move(diag.referenceDepth));
  }

void Renderer::drawTreeHistory(Encoder<CommandBuffer>& cmd, uint8_t fId, WorldView& view,
                                const BenchProbe::HistoryVerifyRequest& request) {
  using Draw = DrawCommands::HistoryDraw;
  auto& commands = view.treeHistoryCommands();
  const bool reset = !treeHistory.valid || !camHistory.continuous || !commands.hasHistory();
  const bool verifyCull = request && request.bough;
  const bool verifyHistory = request && !request.bough;
  const bool saveBoughHiZ = !verifyHistory && commands.hasPendingBoughHiZ();
  const float quantization = zBufferFormat==TextureFormat::Depth16 ? 1.f/65535.f :
                             zBufferFormat==TextureFormat::Depth24x8 ? 1.f/16777215.f : 0.f;
  static const uint32_t diagnosticEvery = [] {
    if(TreeBoughRender::hiZCullEnabled())
      return 0u;
    const auto* env = std::getenv("OPENGOTHIC_BOUGH_HIZ_DIAG_EVERY");
    if(env==nullptr)
      return 0u;
    uint32_t every = 0;
    const auto* end = env+std::strlen(env);
    const auto parsed = std::from_chars(env, end, every);
    return parsed.ec==std::errc() && parsed.ptr==end ? every : 0u;
    }();
  auto& diag = boughHiZDiag;
  const auto* previousBoughHiZ = saveBoughHiZ && !reset && diag.valid ?
                                 &textureCast<const Texture2d&>(diag.finalPyramid) : nullptr;
  const bool sampleBough = diagnosticEvery!=0 && !reset && diag.valid && commands.hasPendingBoughExpansion() &&
                           diag.frames++%diagnosticEvery==0;
  const bool verifyBough = sampleBough && !request && !diag.proved &&
                           (!FrameProfile::enabled() || FrameProfile::hasSample());

  commands.classifyHistory(cmd, textureCast<const Texture2d&>(hiz.hiZ), quantization, false, reset);
  if(!reset)
    commands.compactHistory(cmd, true);

  if(verifyHistory || !reset) {
    cmd.setDebugMarker("HiZ-history-depth");
    if(verifyHistory) {
      auto& owners = usesAttachment(treeHistory.earlyOwners, TextureFormat::R32U, zbuffer.size());
      cmd.setFramebuffer({{owners, Vec4(), Preserve}}, {zbuffer, Preserve, Preserve});
      } else {
      cmd.setFramebuffer({}, {zbuffer, Preserve, Preserve});
      }
    if(!reset) {
      const auto draw = verifyHistory ? Draw::EarlyVerify : Draw::Early;
      commands.drawCommon(cmd, SceneGlobals::V_Main, Material::Solid, draw);
      commands.drawCommon(cmd, SceneGlobals::V_Main, Material::AlphaTest, draw);
      }
    }

  if(verifyBough) {
    // Seed a separate target while the early command list is still available.
    auto& reference = usesZBuffer(diag.referenceDepth, zBufferFormat, uint32_t(zbuffer.w()), uint32_t(zbuffer.h()));
    std::swap(reference, zbuffer);
    drawHiZ(cmd, view);
    cmd.setFramebuffer({}, {zbuffer, Preserve, Preserve});
    commands.drawCommon(cmd, SceneGlobals::V_Main, Material::Solid, Draw::Early);
    commands.drawCommon(cmd, SceneGlobals::V_Main, Material::AlphaTest, Draw::Early);
    std::swap(reference, zbuffer);
    }

  if(verifyCull)
    seedBoughHiZReference(cmd, view, reset);

  if(verifyHistory) {
    cmd.setFramebuffer({});
    cmd.setBinding(0, zbuffer, Sampler::nearest());
    cmd.setBinding(1, usesImage2d(treeHistory.earlyDepth, TextureFormat::R32F, zbuffer.size()));
    cmd.setPipeline(shaders.ssaoDepth);
    cmd.dispatchThreads(zbuffer.size());
    }

  auto pyramidW = RendererInternal::nextPot(uint32_t(zbuffer.w()));
  auto pyramidH = RendererInternal::nextPot(uint32_t(zbuffer.h()));
  while(pyramidW>128 || pyramidH>128) {
    pyramidW = std::max(1u, pyramidW/2);
    pyramidH = std::max(1u, pyramidH/2);
    }
  const auto* grassHiZ = &textureCast<const Texture2d&>(hiz.hiZ);
  if(!reset) {
    auto& pyramid = usesImage2d(treeHistory.pyramid, TextureFormat::R32F, pyramidW, pyramidH, true);
    buildHiZ(cmd, &pyramid);
    grassHiZ = &textureCast<const Texture2d&>(pyramid);
    commands.classifyHistory(cmd, *grassHiZ, quantization, true, false);
    }
  if(previousBoughHiZ==nullptr)
    commands.finishBoughVisibility(cmd, fId, !reset);
  else if(verifyCull)
    commands.finishBoughVisibility(cmd, fId, !reset, nullptr, nullptr, false);
  else
    commands.finishBoughVisibility(cmd, fId, !reset, previousBoughHiZ, &diag.previousViewProject);
  if(!reset)
    commands.compactHistory(cmd, false);

  // The reference pass and the live pass draw the same prepared grass.
  view.prepareGrass(cmd, fId, *grassHiZ, reset ? 0.f : quantization);
  const auto mode = reset ? Draw::Unfiltered : Draw::Filtered;
  if(verifyCull) {
    commands.setBoughHiZVerify(true);
    drawBoughHiZReference(cmd, fId, view, mode);
    if(previousBoughHiZ!=nullptr) {
      cmd.setFramebuffer({});
      commands.filterBoughVisibility(cmd, *previousBoughHiZ, diag.previousViewProject);
      }
    }
  if(verifyHistory) {
    auto& reference = usesZBuffer(treeHistory.referenceDepth, zBufferFormat, uint32_t(zbuffer.w()), uint32_t(zbuffer.h()));
    auto& owners = usesAttachment(treeHistory.referenceOwners, TextureFormat::R32U, zbuffer.size());
    auto& scene = view.sceneGlobals();
    auto& globals = scene.uboGlobal[SceneGlobals::V_Main];
    auto probe = scene.treeHistoryReferenceUbo();
    // Replay the ordinary occluders and pMain on the unfiltered list. The 3a probe
    // records owners in the normal target, so pMain keeps its usual attachments.
    cmd.setGraphicsDescriptorArrayReuse(false);
    std::swap(reference, zbuffer);
    drawHiZ(cmd, view);
    std::swap(owners, gbufNormal);
    std::swap(probe, globals);
    drawGBuffer(cmd, fId, view);
    cmd.setGraphicsDescriptorArrayReuse(view.drawBuckets().textureBankRebuilt());
    std::swap(probe, globals);
    std::swap(owners, gbufNormal);
    std::swap(reference, zbuffer);
    Resources::recycle(std::move(probe));
    }

  size_t proofStride = 0;
  if(verifyBough) {
    // Both GBuffer passes see the same camera, grass, early depth and final draw lists.
    std::swap(diag.referenceDepth, zbuffer);
    drawGBuffer(cmd, fId, view, mode, &treeHistory.winners);
    std::swap(diag.referenceDepth, zbuffer);
    diag.proofPixels = size_t(zbuffer.w())*size_t(zbuffer.h());
    diag.proofPixelBytes = zBufferFormat==TextureFormat::Depth16 ? sizeof(uint16_t) : sizeof(uint32_t);
    const size_t depthBytes = diag.proofPixels*diag.proofPixelBytes;
    proofStride = (depthBytes+3u)&~size_t(3u);
    diag.proofDepth = Resources::device().ssbo(BufferHeap::Readback, Uninitialized, proofStride+depthBytes);
    cmd.setFramebuffer({});
    cmd.copy(textureCast<const Texture2d&>(diag.referenceDepth), 0, diag.proofDepth, 0);
    }
  if(diagnosticEvery!=0 && !diag.finalPyramid.isEmpty()) {
    const auto stamp = sampleBough ? std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      std::chrono::steady_clock::now().time_since_epoch()).count() : 0;
    if(commands.diagnoseBoughVisibility(cmd, fId, textureCast<const Texture2d&>(diag.finalPyramid),
                                        diag.previousViewProject, verifyBough, uint64_t(stamp)) && verifyBough) {
      diag.proofSlot = fId;
      diag.proofStamp = uint64_t(stamp);
      diag.proved = true;
      }
    }

  drawGBuffer(cmd, fId, view, mode, &treeHistory.winners);
  if(previousBoughHiZ!=nullptr) {
    // Only geometry in this frame's cut contributes to the pyramid that can reject a node.
    buildHiZ(cmd, &diag.finalPyramid, true);
    commands.recheckBoughVisibility(cmd, fId, *previousBoughHiZ);
    drawGBuffer(cmd, fId, view, mode, &treeHistory.winners, true);
    }
  if(verifyCull) {
    compareBoughHiZ(cmd, request, reset, previousBoughHiZ!=nullptr);
    commands.setBoughHiZVerify(false);
    }
  if(verifyBough && diag.proofStamp!=0) {
    cmd.setFramebuffer({});
    cmd.copy(textureCast<const Texture2d&>(zbuffer), 0, diag.proofDepth, proofStride);
    }
  if(diagnosticEvery!=0 || saveBoughHiZ) {
    // hiz.hiZ is rebuilt from the early occluders each frame. Preserve the finished
    // opaque depth separately together with the jittered projection that produced it.
    auto& pyramid = usesImage2d(diag.finalPyramid, TextureFormat::R32F, pyramidW, pyramidH, !saveBoughHiZ);
    buildHiZ(cmd, &pyramid, saveBoughHiZ);
    diag.previousViewProject = view.sceneGlobals().viewProject();
    diag.valid = true;
    } else {
    diag.valid = false;
    }
  const auto& winners = textureCast<const Texture2d&>(treeHistory.winners);
  commands.finishHistory(cmd, winners, fId);
  treeHistory.valid = true;
  if(verifyHistory) {
    treeHistory.verify.compare(cmd, shaders.hizHistoryVerify, request,
                               textureCast<const Texture2d&>(treeHistory.earlyDepth),
                               textureCast<const Texture2d&>(treeHistory.earlyOwners),
                               textureCast<const Texture2d&>(treeHistory.referenceDepth),
                               textureCast<const Texture2d&>(treeHistory.referenceOwners),
                               textureCast<const Texture2d&>(zbuffer), winners,
                               commands.historyStatus(), commands.historyClusterCount(), reset);
    }
  }

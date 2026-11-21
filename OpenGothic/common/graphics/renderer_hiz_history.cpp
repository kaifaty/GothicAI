#include "renderer.h"

#include "renderer_internal.h"

using namespace Tempest;

void Renderer::drawTreeHistory(Encoder<CommandBuffer>& cmd, uint8_t fId, WorldView& view,
                                const BenchProbe::HistoryVerifyRequest& request) {
  using Draw = DrawCommands::HistoryDraw;
  auto& commands = view.treeHistoryCommands();
  const bool reset = !treeHistory.valid || !camHistory.continuous || !commands.hasHistory();
  const float quantization = zBufferFormat==TextureFormat::Depth16 ? 1.f/65535.f :
                             zBufferFormat==TextureFormat::Depth24x8 ? 1.f/16777215.f : 0.f;

  commands.classifyHistory(cmd, textureCast<const Texture2d&>(hiz.hiZ), quantization, false, reset);
  if(!reset)
    commands.compactHistory(cmd, true);

  if(request || !reset) {
    cmd.setDebugMarker("HiZ-history-depth");
    if(request) {
      auto& owners = usesAttachment(treeHistory.earlyOwners, TextureFormat::R32U, zbuffer.size());
      cmd.setFramebuffer({{owners, Vec4(), Preserve}}, {zbuffer, Preserve, Preserve});
      } else {
      cmd.setFramebuffer({}, {zbuffer, Preserve, Preserve});
      }
    if(!reset) {
      const auto draw = request ? Draw::EarlyVerify : Draw::Early;
      commands.drawCommon(cmd, SceneGlobals::V_Main, Material::Solid, draw);
      commands.drawCommon(cmd, SceneGlobals::V_Main, Material::AlphaTest, draw);
      }
    }

  if(request) {
    cmd.setFramebuffer({});
    cmd.setBinding(0, zbuffer, Sampler::nearest());
    cmd.setBinding(1, usesImage2d(treeHistory.earlyDepth, TextureFormat::R32F, zbuffer.size()));
    cmd.setPipeline(shaders.ssaoDepth);
    cmd.dispatchThreads(zbuffer.size());
    }

  const auto* currentHiZ = &textureCast<const Texture2d&>(hiz.hiZ);
  if(!reset) {
    auto w = RendererInternal::nextPot(uint32_t(zbuffer.w()));
    auto h = RendererInternal::nextPot(uint32_t(zbuffer.h()));
    while(w>128 || h>128) {
      w = std::max(1u, w/2);
      h = std::max(1u, h/2);
      }
    auto& pyramid = usesImage2d(treeHistory.pyramid, TextureFormat::R32F, w, h, true);
    buildHiZ(cmd, &pyramid);
    currentHiZ = &textureCast<const Texture2d&>(pyramid);
    commands.classifyHistory(cmd, *currentHiZ, quantization, true, false);
    }
  commands.finishBoughVisibility(cmd, fId, reset ? nullptr : currentHiZ);
  if(!reset)
    commands.compactHistory(cmd, false);

  // The reference pass and the live pass draw the same prepared grass.
  view.prepareGrass(cmd, fId, *currentHiZ, reset ? 0.f : quantization);
  if(request) {
    auto& reference = usesZBuffer(treeHistory.referenceDepth, zBufferFormat, uint32_t(zbuffer.w()), uint32_t(zbuffer.h()));
    auto& owners = usesAttachment(treeHistory.referenceOwners, TextureFormat::R32U, zbuffer.size());
    auto& scene = view.sceneGlobals();
    auto& globals = scene.uboGlobal[SceneGlobals::V_Main];
    auto probe = scene.treeHistoryReferenceUbo();
    // Replay the ordinary occluders and pMain on the unfiltered list. The 3a probe
    // records owners in the normal target, so pMain keeps its usual attachments.
    std::swap(reference, zbuffer);
    drawHiZ(cmd, view);
    std::swap(owners, gbufNormal);
    std::swap(probe, globals);
    drawGBuffer(cmd, fId, view);
    std::swap(probe, globals);
    std::swap(owners, gbufNormal);
    std::swap(reference, zbuffer);
    Resources::recycle(std::move(probe));
    }

  drawGBuffer(cmd, fId, view, reset ? Draw::Unfiltered : Draw::Filtered, &treeHistory.winners);
  const auto& winners = textureCast<const Texture2d&>(treeHistory.winners);
  commands.finishHistory(cmd, winners, fId);
  treeHistory.valid = true;
  if(request) {
    treeHistory.verify.compare(cmd, shaders.hizHistoryVerify, request,
                               textureCast<const Texture2d&>(treeHistory.earlyDepth),
                               textureCast<const Texture2d&>(treeHistory.earlyOwners),
                               textureCast<const Texture2d&>(treeHistory.referenceDepth),
                               textureCast<const Texture2d&>(treeHistory.referenceOwners),
                               textureCast<const Texture2d&>(zbuffer), winners,
                               commands.historyStatus(), commands.historyClusterCount(), reset);
    }
  }

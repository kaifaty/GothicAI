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

  if(!reset) {
    auto w = RendererInternal::nextPot(uint32_t(zbuffer.w()));
    auto h = RendererInternal::nextPot(uint32_t(zbuffer.h()));
    while(w>128 || h>128) {
      w = std::max(1u, w/2);
      h = std::max(1u, h/2);
      }
    auto& pyramid = usesImage2d(treeHistory.pyramid, TextureFormat::R32F, w, h, true);
    buildHiZ(cmd, &pyramid);
    commands.classifyHistory(cmd, textureCast<const Texture2d&>(pyramid), quantization, true, false);
    commands.compactHistory(cmd, false);
    }

  if(request) {
    auto& reference = usesZBuffer(treeHistory.referenceDepth, zBufferFormat, uint32_t(zbuffer.w()), uint32_t(zbuffer.h()));
    auto& owners = usesAttachment(treeHistory.referenceOwners, TextureFormat::R32U, zbuffer.size());
    // Both draws use the same original list and resources this frame. The reference's
    // depth starts at the ordinary occluder pass, independent of all history writes.
    std::swap(reference, zbuffer);
    drawHiZ(cmd, view);
    drawGBuffer(cmd, fId, view, Draw::Reference, &owners);
    std::swap(reference, zbuffer);
    }

  drawGBuffer(cmd, fId, view, reset ? Draw::Reference : Draw::Filtered, &treeHistory.winners);
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

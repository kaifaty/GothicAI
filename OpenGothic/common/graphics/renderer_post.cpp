#include "renderer.h"

#include <Tempest/Log>
#include <cstdio>

#include "camera.h"
#include "gothic.h"

using namespace Tempest;

void Renderer::prepareTaa(WorldView& wview, const Camera& camera) {
  // the path tracer accumulates its own frames, with a camera that stands still
  // the temporal upscale (graphics plan 1g) takes the jitter and the history of the TAA in its own way
  temporal.active = settings.taaEnabled && !settings.pathTraceEnabled && !temporal.output.isEmpty();
  taa.active      = settings.taaEnabled && !settings.pathTraceEnabled && (!taa.output.isEmpty() || temporal.active);
  if(!taa.active) {
    taa.valid = false;
    wview.setJitter(nullptr);
    wview.setMipBias(0);
    wview.setTaaFrame(false, 0);
    return;
    }
  wview.setMipBias(temporal.active ? temporal.mipBias : settings.taaMipBias);

  // without jitter, as the camera gives it; the shadows (Camera::viewShadow*) are made from it too
  Taa::View cur;
  cur.proj    = proj;
  cur.viewLwc = camera.viewLwc();
  cur.origin  = camera.originLwc();
  if(camera.cuts()!=taa.cuts || Taa::isCameraJump(cur, taa.prev))
    taa.valid = false;
  taa.cuts = camera.cuts();

  wview.setTaaFrame(settings.vsmLampTaa, taa.frame);
  const Vec2 px = temporal.active ? Taa::jitterPx(taa.frame++, temporal.phases) : Taa::jitterPx(taa.frame++);
  const auto j  = Taa::jitterMatrix(px, uint32_t(zbuffer.w()), uint32_t(zbuffer.h()));
  taa.jitter    = px;
  if(taa.valid)
    taa.reproject = Taa::reprojection(cur, taa.prev, j);
  taa.prev = cur;

  // the main view only: the frustum, SSAO and everything in SceneGlobals; not the shadows, UI or inventory
  auto m = j; m.mul(proj);        proj        = m;
  m      = j; m.mul(viewProj);    viewProj    = m;
  m      = j; m.mul(viewProjLwc); viewProjLwc = m;
  wview.setJitter(&j);
  }

void Renderer::drawBloom(Encoder<CommandBuffer>& cmd, bool fused) {
  if(!settings.bloomEnabled || bloomChain[0].isEmpty())
    return;
  cmd.setDebugMarker("Bloom");
  // down: the scene to half resolution (Karis-weighted), then each level to the next
  for(size_t i=0; i<bloomChain.size(); ++i) {
    cmd.setFramebuffer({{bloomChain[i], Tempest::Discard, Tempest::Preserve}});
    if(i==0 && temporal.active)
      cmd.setBinding(0, temporal.output, Sampler::bilinear(ClampMode::ClampToEdge));
    else if(i==0 && fused)
      cmd.setBinding(0, taa.history[taa.cur^1], Sampler::bilinear(ClampMode::ClampToEdge));
    else
      cmd.setBinding(0, i==0 ? sceneLinear : bloomChain[i-1], Sampler::bilinear(ClampMode::ClampToEdge));
    cmd.setPipeline(i==0 ? shaders.bloomDownKaris : shaders.bloomDown);
    cmd.draw(nullptr, 0, 3);
    }
  // up: each level adds its tent-filtered smaller one, so the half level ends up with all of them
  for(size_t i=bloomChain.size()-1; i>0; --i) {
    cmd.setFramebuffer({{bloomChain[i-1], Tempest::Preserve, Tempest::Preserve}});
    cmd.setBinding(0, bloomChain[i], Sampler::bilinear(ClampMode::ClampToEdge));
    cmd.setPipeline(shaders.bloomUp);
    cmd.draw(nullptr, 0, 3);
    }
  if(temporal.active || fused)
    return; // the tonemapping mixes it in (drawTonemappingTemporal, drawTonemappingTaa)
  struct Push {
    float strength = 0.05f;  // the share of the scene's light a lens scatters into the glow
    float levels   = 0;
    } push;
  push.levels = float(bloomChain.size());
  cmd.setFramebuffer({{sceneLinear, Tempest::Preserve, Tempest::Preserve}});
  cmd.setBinding(0, bloomChain[0], Sampler::bilinear(ClampMode::ClampToEdge));
  cmd.setPushData(push);
  cmd.setPipeline(shaders.bloomMix);
  cmd.draw(nullptr, 0, 3);
  }

void Renderer::drawTonemapping(Attachment& result, Encoder<CommandBuffer>& cmd, const WorldView& wview) {
  struct Push {
    float brightness = 0;
    float contrast   = 1;
    float gamma      = 1.f/2.2f;
    float mul        = 1;
    };

  Push p;
  p.brightness = (settings.zVidBrightness - 0.5f)*0.1f;
  p.contrast   = std::max(1.5f - settings.zVidContrast, 0.01f);
  p.gamma      = p.gamma/std::max(2.0f*settings.zVidGamma,  0.01f);

  static float mul = 0.f;
  if(mul>0)
    p.mul = mul;

  const bool native = (result.size()==sceneLinear.size());
  auto& pso = native ? shaders.tonemapping : shaders.tonemappingUpscale;
  cmd.setFramebuffer({ {result, Tempest::Discard, Tempest::Preserve} });
  cmd.setBinding(0, wview.sceneGlobals().uboGlobal[SceneGlobals::V_Main]);
  // This Lanczos implementation pairs adjacent weights using bilinear sampling.
  // Keep nearest sampling at native resolution.
  const auto sampler = native ? Sampler::nearest(ClampMode::ClampToEdge) :
                                Sampler::bilinear(ClampMode::ClampToEdge);
  cmd.setBinding(1, sceneLinear, sampler);
  cmd.setPushData(p);
  cmd.setPipeline(pso);
  cmd.draw(nullptr, 0, 3);
  }

void Renderer::drawCMAA2(Tempest::Attachment& result, Tempest::Encoder<Tempest::CommandBuffer>& cmd, const WorldView& wview) {
  auto&          pso             = shaders.cmaa2EdgeColor2x2Presets[Gothic::options().aaPreset];
  const IVec3    inputGroupSize  = pso.workGroupSize();
  const IVec3    outputGroupSize = inputGroupSize - IVec3(2, 2, 0);
  const uint32_t groupCountX     = uint32_t((sceneLinear.w() + outputGroupSize.x * 2 - 1) / (outputGroupSize.x * 2));
  const uint32_t groupCountY     = uint32_t((sceneLinear.h() + outputGroupSize.y * 2 - 1) / (outputGroupSize.y * 2));

  cmd.setFramebuffer({});
  cmd.setBinding(0, sceneLinear, Sampler::bilinear(ClampMode::ClampToEdge));
  cmd.setBinding(1, cmaa2.workingEdges);
  cmd.setBinding(2, cmaa2.shapeCandidates);
  cmd.setBinding(3, cmaa2.deferredBlendLocationList);
  cmd.setBinding(4, cmaa2.deferredBlendItemList);
  cmd.setBinding(5, cmaa2.deferredBlendItemListHeads);
  cmd.setBinding(6, cmaa2.controlBuffer);
  cmd.setBinding(7, cmaa2.indirectBuffer);

  // detect edges
  cmd.setPipeline(pso);
  cmd.dispatch(groupCountX, groupCountY, 1);

  // process candidates pass
  cmd.setPipeline(shaders.cmaa2ProcessCandidates);
  cmd.dispatchIndirect(cmaa2.indirectBuffer, 0);

  // deferred color apply
  struct Push {
    float brightness = 0;
    float contrast   = 1;
    float gamma      = 1.f/2.2f;
    float mul        = 1;
    };

  Push p;
  p.brightness = (settings.zVidBrightness - 0.5f)*0.1f;
  p.contrast   = std::max(1.5f - settings.zVidContrast, 0.01f);
  p.gamma      = p.gamma/std::max(2.0f*settings.zVidGamma,  0.01f);

  static float mul = 0.f;
  if(mul>0)
    p.mul = mul;

  // CMAA2 writes pixels in sceneLinear coordinates: result must have the same size
  auto& psoTone = shaders.tonemapping;
  cmd.setFramebuffer({{result, Tempest::Discard, Tempest::Preserve}});
  cmd.setBinding(0, wview.sceneGlobals().uboGlobal[SceneGlobals::V_Main]);
  cmd.setBinding(1, sceneLinear, Sampler::nearest());
  cmd.setPushData(&p, sizeof(p));
  cmd.setPipeline(psoTone);
  cmd.draw(nullptr, 0, 3);

  cmd.setBinding(0, sceneLinear);
  cmd.setBinding(1, cmaa2.workingEdges);
  cmd.setBinding(2, cmaa2.shapeCandidates);
  cmd.setBinding(3, cmaa2.deferredBlendLocationList);
  cmd.setBinding(4, cmaa2.deferredBlendItemList);
  cmd.setBinding(5, cmaa2.deferredBlendItemListHeads);
  cmd.setBinding(6, cmaa2.controlBuffer);
  cmd.setPushData(&p, sizeof(p));
  cmd.setPipeline(shaders.cmaa2DeferredColorApply2x2);
  cmd.drawIndirect(cmaa2.indirectBuffer, 3*sizeof(uint32_t));
  }

void Renderer::drawTaa(Tempest::Encoder<Tempest::CommandBuffer>& cmd, bool fused) {
  // In linear HDR after the translucent and underwater passes, before bloom (shader/antialiasing/taa_resolve.frag)
  struct Push {
    Matrix4x4 reproject;
    float     alpha = 0.1f; // the share of the new frame
    uint32_t  reset = 0;    // no history: the frame as it is
    } push;
  push.reproject = taa.reproject;
  push.reset     = taa.valid ? 0 : 1;

  auto& next = taa.history[taa.cur];
  auto& prev = taa.history[taa.cur^1];
  cmd.setDebugMarker("TAA");
  cmd.setFramebuffer({{next, Tempest::Discard, Tempest::Preserve}});
  cmd.setBinding(0, sceneLinear, Sampler::nearest(ClampMode::ClampToEdge));
  cmd.setBinding(1, zbuffer,     Sampler::nearest(ClampMode::ClampToEdge));
  cmd.setBinding(2, prev,        Sampler::bilinear(ClampMode::ClampToEdge));
  cmd.setPushData(push);
  cmd.setPipeline(shaders.taaResolve);
  cmd.draw(nullptr, 0, 3);
  taa.valid = true;

  if(fused) {
    taa.cur ^= 1; // the frame stays in the history: drawTonemappingTaa sharpens it on the way out
    return;
    }

  // the history stays unsharpened: sharpening fed back would build up from frame to frame
  drawTaaSharpen(cmd);

  // bloom and tonemapping go on from the resolved frame
  std::swap(sceneLinear, taa.output);
  taa.cur ^= 1;
  }

void Renderer::drawTemporalUpscale(Tempest::Encoder<Tempest::CommandBuffer>& cmd) {
  // In linear HDR after the translucent and underwater passes, where the TAA resolve would be (graphics plan 1g).
  // The camera's motion of each pixel first: from its depth to where it was a frame ago (motion_camera.frag)
  struct Push {
    Matrix4x4 reproject;
    } push;
  push.reproject = taa.valid ? taa.reproject : Matrix4x4::mkIdentity();
  cmd.setDebugMarker("Upscale-motion");
  cmd.setFramebuffer({{temporal.motion, Tempest::Discard, Tempest::Preserve}});
  cmd.setBinding(0, zbuffer, Sampler::nearest(ClampMode::ClampToEdge));
  cmd.setPushData(push);
  cmd.setPipeline(shaders.motionCamera);
  cmd.draw(nullptr, 0, 3);

  // Apple's conventions: the jitter is the pixel's centre minus where it was sampled, in input pixels: the picture
  // moved by +jitter (Taa::jitterMatrix), so the centre shows the point at centre - jitter. The vectors are in the
  // input's texture coordinates, the scale makes pixels of them. The depth is not reversed: the sky is 1.
  // OPENGOTHIC_UPSCALE_JITTER_SIGN=<x>,<y> (a study of the convention): multipliers of the jitter given to MetalFX
  static const Vec2 sign = [] {
    Vec2 s(1, 1);
    if(const char* e = std::getenv("OPENGOTHIC_UPSCALE_JITTER_SIGN"))
      if(std::sscanf(e, "%f,%f", &s.x, &s.y)==2)
        Log::i("render: temporal upscale jitter sign = ", s.x, ",", s.y);
    return s;
    }();
  TemporalUpscaleDesc desc;
  desc.jitterX       = taa.jitter.x*sign.x;
  desc.jitterY       = taa.jitter.y*sign.y;
  desc.motionScaleX  = float(temporal.motion.w());
  desc.motionScaleY  = float(temporal.motion.h());
  desc.inputW        = uint32_t(sceneLinear.w());
  desc.inputH        = uint32_t(sceneLinear.h());
  desc.reset         = !taa.valid;
  desc.depthReversed = false;
  cmd.setFramebuffer({});
  cmd.setDebugMarker("Upscale-temporal");
  cmd.upscaleTemporal(sceneLinear, zbuffer, temporal.motion, temporal.output, desc);
  taa.valid = true;
  }

void Renderer::drawTonemappingTaa(Attachment& result, Encoder<CommandBuffer>& cmd, const WorldView& wview) {
  // drawTaaSharpen, the mix of drawBloom and drawTonemapping in one pass (tonemapping.frag BLOOM, RCAS)
  struct Push {
    float brightness    = 0;
    float contrast      = 1;
    float gamma         = 1.f/2.2f;
    float mul           = 1;
    float bloomStrength = 0.05f; // as drawBloom's mix
    float bloomLevels   = 1;
    float sharpness     = 0;
    } p;
  p.brightness  = (settings.zVidBrightness - 0.5f)*0.1f;
  p.contrast    = std::max(1.5f - settings.zVidContrast, 0.01f);
  p.gamma       = p.gamma/std::max(2.0f*settings.zVidGamma,  0.01f);
  p.bloomLevels = float(bloomChain.size());
  p.sharpness   = settings.taaSharpness;

  cmd.setFramebuffer({ {result, Tempest::Discard, Tempest::Preserve} });
  cmd.setBinding(0, wview.sceneGlobals().uboGlobal[SceneGlobals::V_Main]);
  cmd.setBinding(1, taa.history[taa.cur^1], Sampler::nearest(ClampMode::ClampToEdge));
  cmd.setBinding(2, bloomChain[0], Sampler::bilinear(ClampMode::ClampToEdge));
  cmd.setPushData(p);
  cmd.setPipeline(shaders.tonemappingTaa);
  cmd.draw(nullptr, 0, 3);
  }

void Renderer::drawTonemappingTemporal(Attachment& result, Encoder<CommandBuffer>& cmd, const WorldView& wview) {
  // drawTonemapping at the output resolution, from the upscaled frame; the bloom goes in here (tonemapping.frag BLOOM)
  struct Push {
    float brightness    = 0;
    float contrast      = 1;
    float gamma         = 1.f/2.2f;
    float mul           = 1;
    float bloomStrength = 0.05f; // as drawBloom's mix
    float bloomLevels   = 1;
    } p;
  p.brightness = (settings.zVidBrightness - 0.5f)*0.1f;
  p.contrast   = std::max(1.5f - settings.zVidContrast, 0.01f);
  p.gamma      = p.gamma/std::max(2.0f*settings.zVidGamma,  0.01f);

  const bool bloom = settings.bloomEnabled && !bloomChain[0].isEmpty();
  cmd.setFramebuffer({ {result, Tempest::Discard, Tempest::Preserve} });
  cmd.setBinding(0, wview.sceneGlobals().uboGlobal[SceneGlobals::V_Main]);
  cmd.setBinding(1, temporal.output, Sampler::nearest(ClampMode::ClampToEdge));
  if(bloom) {
    p.bloomLevels = float(bloomChain.size());
    cmd.setBinding(2, bloomChain[0], Sampler::bilinear(ClampMode::ClampToEdge));
    cmd.setPushData(p);
    cmd.setPipeline(shaders.tonemappingBloom);
    } else {
    cmd.setPushData(&p, 4*sizeof(float));
    cmd.setPipeline(shaders.tonemapping);
    }
  cmd.draw(nullptr, 0, 3);
  }

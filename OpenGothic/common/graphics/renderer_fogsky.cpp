#include "renderer.h"

#include <cmath>

#include "camera.h"
#include "gameservices.h"

using namespace Tempest;

// The sky seen from the camera for the fog under GI (fog_sky_vis.comp, fog.frag FOG_SKY_VIS; step-2-shade.md,
// «Дымка в тени», Д4): where the probes' GI runs or the path tracer (the reference shares the fog), with the
// volumetric fog and the ray scene. Elsewhere scene.fogSkyVis stays 1 and the fog takes its own pipeline as before.
bool Renderer::fogSkyActive() const {
  if(shaders.fogSkyVis.isEmpty() || sky.quality!=VolumetricHQ)
    return false;
  const bool gi = settings.giMethod==GiMethod::Probes && settings.zCloudShadowScale;
  return gi || settings.pathTraceEnabled;
  }

void Renderer::updateFogSky(const Camera& camera) {
  // tau of the mean over time: a step under a crown changes the fog over half a second, not at once
  static constexpr float Tau = 0.5f;
  const uint64_t tick = GameServices::inst().worldTickCount();
  const Vec3     cam  = camera.listenerPosition().pos;
  auto& f = fogSky;
  // the pass runs this frame only if prepareFogSkyVis dispatches it: a frame it did not run in (no ray scene yet, the
  // pass off) leaves the state buffer stale or, after its creation, uninitialised; the next run starts over
  const bool ranLast = f.ranLast;
  f.ranLast = false;
  if(!fogSkyActive()) {
    f.hasLast = false;
    return;
    }
  // no way, a jump to this frame's share: the first run, a frame without the pass before it, the clock going back,
  // a teleport or a cut moving the camera more than 3 m in a frame (as EyeAdaptation)
  const bool jump = !ranLast || !f.hasLast || tick<f.lastTick || (cam-f.lastCam).length()>300.f;
  const float dt  = jump ? 0.f : float(tick-f.lastTick)/1000.f;
  f.reset    = jump ? 1 : 0;
  f.alpha    = 1.f - std::exp(-dt/Tau);
  f.hasLast  = true;
  f.lastTick = tick;
  f.lastCam  = cam;
  f.frame++;
  }

void Renderer::prepareFogSkyVis(Encoder<CommandBuffer>& cmd, WorldView& wview) {
  auto& scene = wview.sceneGlobals();
  if(!fogSkyActive() || scene.rtScene.tlas.isEmpty())
    return;
  if(fogSky.state.isEmpty())
    fogSky.state = Resources::device().ssbo(Uninitialized, 2*sizeof(float));
  struct Push {
    float    alpha;
    uint32_t reset;
    uint32_t frame;
    } push = {fogSky.alpha, fogSky.reset, fogSky.frame};
  cmd.setFramebuffer({});
  cmd.setDebugMarker("Fog-sky");
  cmd.setBinding(0, scene.uboGlobal[SceneGlobals::V_Main]);
  cmd.setBinding(1, fogSky.state);
  cmd.setBinding(6, scene.rtScene.tlas);
  cmd.setBinding(7, Sampler::bilinear());
  cmd.setBinding(8, scene.rtScene.tex);
  cmd.setBinding(9, scene.rtScene.vbo);
  cmd.setBinding(10,scene.rtScene.ibo);
  cmd.setBinding(11,scene.rtScene.rtDesc);
  cmd.setPushData(push);
  cmd.setPipeline(shaders.fogSkyVis);
  cmd.dispatch(1);
  fogSky.ranLast = true;
  }

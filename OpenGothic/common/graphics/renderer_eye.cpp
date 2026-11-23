#include "renderer.h"

#include "camera.h"
#include "gameservices.h"

using namespace Tempest;

// The eye's adaptation under GI (eyeadaptation.h): the probes' GI is on, not the path tracer, -eye/OPENGOTHIC_EYE on
bool Renderer::eyeActive() const {
  static const bool enabled = EyeAdaptation::enabledByDefault();
  return enabled && settings.giMethod==GiMethod::Probes && settings.zCloudShadowScale && !settings.pathTraceEnabled;
  }

void Renderer::updateEye(uint8_t fId, const Camera& camera) {
  if(!eyeActive()) {
    eye.off();
    return;
    }
  eye.update(fId, GameServices::inst().worldTickCount(), GameServices::inst().worldTime(), camera.listenerPosition().pos);
  }

void Renderer::drawEyeMeter(Encoder<CommandBuffer>& cmd, uint8_t fId, const WorldView& wview) {
  if(!eyeActive())
    return;
  cmd.setFramebuffer({});
  cmd.setDebugMarker("Eye-meter");
  cmd.setBinding(0, sceneLinear, Sampler::bilinear(ClampMode::ClampToEdge));
  cmd.setBinding(1, zbuffer,     Sampler::nearest(ClampMode::ClampToEdge));
  cmd.setBinding(2, wview.sceneGlobals().uboGlobal[SceneGlobals::V_Main]);
  cmd.setBinding(4, gbufDiffuse, Sampler::bilinear(ClampMode::ClampToEdge));
  cmd.setBinding(3, eye.result(fId));
  cmd.setPipeline(shaders.eyeMeter);
  cmd.dispatch(1);
  eye.markWritten(fId);
  }

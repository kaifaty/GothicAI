#version 450
#extension GL_ARB_separate_shader_objects : enable
#extension GL_GOOGLE_include_directive : enable

#include "scene.glsl"
#include "common.glsl"
#include "grass/grass_common.glsl"

// A blade's pixel in the G-buffer (reports/engine/2026-11-14-grass-system): as a leaf's (GBufFoliageBit, the
// share of the sun through it in the channel of metal), an own material by its mark. Needs the material maps'
// attachment: without it there is no grass.

layout(binding = 0, std140) uniform UboScene {
  SceneDesc scene;
  };

layout(location = 0) in vec3 inColor;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inShade;
layout(location = 3) in vec3 inPrevStatic;
layout(location = 4) in vec3 inPrevDelta;

layout(location = 0) out vec4 outDiffuse;
layout(location = 1) out uint outNormal;
layout(location = 2) out vec4 outOrm;
layout(location = 3) out vec2 outMotion;

void main() {
  outDiffuse = vec4(inColor, float(GBufFoliageBit)/255.0);
  outNormal  = encodeNormal(normalize(inNormal));
  outOrm     = vec4(inShade.x, GrassRoughness, inShade.y, 1.0);

  // as motionResidual of materials/main.frag
  outMotion = vec2(0);
  if(scene.motionVectors!=0u) {
    const float w0 = inPrevStatic.z;
    const float w1 = inPrevStatic.z + inPrevDelta.z;
    if(w0>0.0 && w1>0.0)
      outMotion = (inPrevDelta.xy*w0 - inPrevStatic.xy*inPrevDelta.z)/(w0*w1)*0.5;
    }
  }

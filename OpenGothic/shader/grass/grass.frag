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

layout(location = 0) in vec4 inColor;     // w: the share of the sward's own palette
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inShade;     // the share of the blade's length, of the sun through, the dryness, the sheen
layout(location = 3) in vec3 inPrevStatic;
layout(location = 4) in vec3 inPrevDelta;

layout(location = 0) out vec4 outDiffuse;
layout(location = 1) out uint outNormal;
layout(location = 2) out vec4 outOrm;
layout(location = 3) out vec2 outMotion;

void main() {
  // Darker at the root, lighter and greener or more like straw at the tip. Counted here, not per vertex: a blade
  // of one link far away has the tone of a blade of four
  const uint  partsOff = grassPartsOff(scene.grassRing.w);
  const float t        = clamp(inShade.x, 0.0, 1.0);
  const float tone     = grassTone(t);
  // a blade of a ground with its own sward takes that sward's colour, which is grass already: the tip barely
  // shifts it, so the blades and the ground between them are of one palette
  const float own      = inColor.a;
  const vec3  tip      = (partsOff & GrassPartTip)!=0u ? vec3(1.0)
                         : mix(mix(GrassTipGreen, GrassTipStraw, inShade.z), mix(GrassOwnTipGreen, GrassOwnTipStraw, inShade.z), own);
  const float root     = (partsOff & GrassPartRoot)!=0u ? 1.0 : mix(GrassRootShade, GrassOwnRootShade, own);
  vec3  color   = min(inColor.rgb*mix(vec3(root), tip, tone)*(1.0 + inShade.w*t), vec3(1.0));
  float ao      = (partsOff & GrassPartRootAo)!=0u ? 1.0 : mix(GrassRootAo, 1.0, tone);
  float through = inShade.y;
  if((partsOff & GrassPartPaint)!=0u) {
    color   = vec3(1,0,0);
    ao      = 1.0;
    through = 0.0;
    }
  outDiffuse = vec4(color, float(GBufFoliageBit)/255.0);
  outNormal  = encodeNormal(normalize(inNormal));
  outOrm     = vec4(ao, GrassRoughness, through, 1.0);

  // as motionResidual of materials/main.frag
  outMotion = vec2(0);
  if(scene.motionVectors!=0u) {
    const float w0 = inPrevStatic.z;
    const float w1 = inPrevStatic.z + inPrevDelta.z;
    if(w0>0.0 && w1>0.0)
      outMotion = (inPrevDelta.xy*w0 - inPrevStatic.xy*inPrevDelta.z)/(w0*w1)*0.5;
    }
  }

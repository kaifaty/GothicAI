#version 450
#extension GL_ARB_separate_shader_objects : enable
#extension GL_GOOGLE_include_directive : enable

#include "scene.glsl"
#include "common.glsl"
#include "grass/grass_common.glsl"
#include "grass/grass_ear.glsl"

// A blade's pixel in the G-buffer (reports/engine/2026-11-14-grass-system): as a leaf's (GBufFoliageBit, the
// share of the sun through it in the channel of metal), an own material by its mark. Needs the material maps'
// attachment: without it there is no grass.

layout(binding = 0, std140) uniform UboScene {
  SceneDesc scene;
  };

layout(location = 0) in vec4 inColor;     // the painted ground under the root; w: 1 on a ground with its own layers,
                                          // 2 a stalk with an ear
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec4 inShade;     // the share of the blade's length, of the sun through, the dryness, the sheen
layout(location = 3) in vec3 inPrevStatic;
layout(location = 4) in vec3 inPrevDelta;
layout(location = 5) in vec3 inRoot;      // on a ground with its own layers: the sward under the root

layout(location = 0) out vec4 outDiffuse;
layout(location = 1) out uint outNormal;
layout(location = 2) out vec4 outOrm;
layout(location = 3) out vec2 outMotion;
#if defined(HISTORY_OWNER)
layout(location = 4) out uint outHistoryOwner;
#endif

void main() {
  // Darker at the root, lighter and greener or more like straw at the tip. Counted here, not per vertex: a blade
  // of one link far away has the tone of a blade of four
  const uint  partsOff = grassPartsOff(scene.grassRing.w);
  const float t        = clamp(inShade.x, 0.0, 1.0);
  // a stand of stalks is deep: the light reaches the ears, the stalks under them stand in each other's shade
  const float tone     = inColor.a>1.5 ? pow(t, GrassEarDepth) : grassTone(t);
  const vec3  tip      = (partsOff & GrassPartTip)!=0u ? vec3(1.0) :
                         (inColor.a>1.5 ? grassEarTip(t, inShade.z) : mix(GrassTipGreen, GrassTipStraw, inShade.z));
  const float root     = (partsOff & GrassPartRoot)!=0u ? 1.0 : GrassRootShade;
  // on a ground with its own layers the root is of the sward the blade stands in, so the blade grows out of its
  // ground; the tip is what it is on a painted ground: green or straw by the blade, over the painted colours
  vec3  color   = inColor.a>0.5 ? mix(inRoot*root, inColor.rgb*tip, tone) : inColor.rgb*mix(vec3(root), tip, tone);
  color         = min(color*(1.0 + inShade.w*t), vec3(1.0));
  float ao      = (partsOff & GrassPartRootAo)!=0u ? 1.0 : mix(GrassRootAo, 1.0, tone);
  float through = inShade.y;
  if((partsOff & GrassPartPaint)!=0u) {
    color   = vec3(1,0,0);
    ao      = 1.0;
    through = 0.0;
    }
  outDiffuse = vec4(color, float(GBufFoliageBit)/255.0);
  outNormal  = (scene.viewFlags & VF_TREE_OCCLUSION_PROBE)!=0u ? 0u : encodeNormal(normalize(inNormal));
  outOrm     = vec4(ao, GrassRoughness, through, 1.0);
#if defined(HISTORY_OWNER)
  outHistoryOwner = 0u;
#endif

  // as motionResidual of materials/main.frag
  outMotion = vec2(0);
  if(scene.motionVectors!=0u) {
    const float w0 = inPrevStatic.z;
    const float w1 = inPrevStatic.z + inPrevDelta.z;
    if(w0>0.0 && w1>0.0)
      outMotion = (inPrevDelta.xy*w0 - inPrevStatic.xy*inPrevDelta.z)/(w0*w1)*0.5;
    }
  }

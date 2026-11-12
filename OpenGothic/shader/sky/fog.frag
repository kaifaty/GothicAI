#version 450
#extension GL_ARB_separate_shader_objects : enable
#extension GL_GOOGLE_include_directive    : enable
#extension GL_EXT_control_flow_attributes : enable

#if defined(VIRTUAL_SHADOW)
#include "virtual_shadow/vsm_common.glsl"
#endif

#include "sky_common.glsl"
#include "scene.glsl"

#if defined(GL_COMPUTE_SHADER)
layout(local_size_x = 1*8, local_size_y = 2*8) in;
const uint NumThreads = gl_WorkGroupSize.x*gl_WorkGroupSize.y*gl_WorkGroupSize.z;
vec2 inPos;
#else
vec2 inPos;
layout(location = 0) out vec4 outColor;
#endif

#if defined(GL_COMPUTE_SHADER)
// none
#else
layout(binding = 0) uniform sampler3D fogLut;
layout(binding = 1) uniform sampler3D fogLutMs;
#endif

layout(binding = 2) uniform sampler2D depth;

layout(binding = 3, std140) uniform UboScene {
  SceneDesc scene;
  };

vec3 inverse(vec3 pos) {
  vec4 ret = scene.viewProjectInv*vec4(pos,1.0);
  return (ret.xyz/ret.w)/100.f;
  }

#if defined(VOLUMETRIC) && defined(GL_COMPUTE_SHADER)
layout(binding = 4, r32ui) uniform writeonly restrict uimage2D occlusionLut;
#elif defined(VOLUMETRIC)
layout(binding = 4, r32ui) uniform readonly  restrict uimage2D occlusionLut;
#endif

#if defined(VOLUMETRIC) && !defined(VIRTUAL_SHADOW) && !defined(SHADOW_EMPTY) && defined(GL_COMPUTE_SHADER)
layout(binding = 5) uniform sampler2D textureSm1;
#endif

#if defined(VOLUMETRIC) && defined(VIRTUAL_SHADOW) && defined(GL_COMPUTE_SHADER)
layout(binding = 5) uniform utexture3D pageTbl;
layout(binding = 6) uniform texture2D  pageData;
#endif

#if defined(GL_COMPUTE_SHADER)
uvec2 invocationID = gl_GlobalInvocationID.xy;
#endif

float interleavedGradientNoise() {
#if defined(GL_COMPUTE_SHADER)
  return interleavedGradientNoise(invocationID.xy);
#else
  return interleavedGradientNoise(gl_FragCoord.xy);
#endif
  }

#if defined(VOLUMETRIC) && defined(GL_COMPUTE_SHADER)
float shadowSample(in sampler2D shadowMap, vec2 shPos) {
  shPos.xy = shPos.xy*vec2(0.5)+vec2(0.5);
  return textureLod(shadowMap,shPos,0).r;
  }

bool calcShadow(vec3 shPos) {
#if defined(SHADOW_EMPTY)
  // the frame cleared the map and drew nothing into it (sun below Camera::minShadowY): every texel is 0
  const float sh = 0.0;
#else
  float sh = shadowSample(textureSm1,shPos.xy);
#endif
  return sh < shPos.z;
  }

bool shadowFactor(vec4 shPos) {
  if(abs(shPos.x)>=shPos.w || abs(shPos.y)>=shPos.w)
    return true;
  return calcShadow(shPos.xyz/shPos.w);
  }
#else
bool shadowFactor(vec4 shPos) {
  return true;
  }
#endif

vec3 project(mat4 m, vec3 pos) {
  vec4 p = m*vec4(pos,1);
  return p.xyz/p.w;
  }

#if defined(VOLUMETRIC)
vec4 fog(vec2 uv, float z) {
  const int   steps    = 32;
  const float noise    = interleavedGradientNoise()/steps;

  const vec3  pos0     = project(scene.viewProjectLwcInv, vec3(inPos,dFogMin));
  const vec3  pos1     = project(scene.viewProjectLwcInv, vec3(inPos,dFogMax));
  const vec3  posz     = project(scene.viewProjectLwcInv, vec3(inPos,z));

#if defined(VIRTUAL_SHADOW)
  const vec4  shPos0   = scene.viewVirtualShadowLwc*vec4(pos0, 1);
  const vec4  shPos1   = scene.viewVirtualShadowLwc*vec4(posz, 1);
#else
  const vec4  shPos0   = scene.viewShadowLwc[1]*vec4(pos0, 1);
  const vec4  shPos1   = scene.viewShadowLwc[1]*vec4(posz, 1);
#endif

  const vec3  ray      = pos1.xyz - pos0.xyz;
  const float dist     = length(ray)*0.01;       // meters
  const float distZ    = length(posz-pos0)*0.01; // meters

  // float d    = (dZ-d0)/(d1-d0);
  // return vec4(debugColors[min(int(d*textureSize(fogLut,0).z), textureSize(fogLut,0).z-1)%MAX_DEBUG_COLORS], 1);

#if defined(GL_COMPUTE_SHADER)
  uint occlusion = 0;
  [[dont_unroll]]
  for(uint i=0; i<steps; ++i) {
    float t      = (i+0.5)/float(steps);
    vec4  shPos  = mix(shPos0,shPos1,t+noise);
    bool  shadow = shadowFactor(shPos);
    occlusion = occlusion | ((shadow ? 1u : 0u) << uint(i));
    }
#else
  vec3  scatteredLight = vec3(0.0);
  vec4  prevVal        = vec4(0.0); //textureLod(fogLut, vec3(uv,0), 0);
#if defined(FOG_SKY_VIS)
  vec3  msShadowed     = vec3(0.0);
  vec4  prevMs         = vec4(0.0);
#endif
#endif

#if defined(GL_COMPUTE_SHADER)
  imageStore(occlusionLut, ivec2(invocationID.xy), uvec4(occlusion));
  return vec4(0);
#else
  // every bit = one sample of shadowmap; a LUT texel covers occlusionRatio pixels (halfResAoFog)
  const ivec2 occlusionSize  = imageSize(occlusionLut);
  const ivec2 occlusionRatio = (textureSize(depth,0) + occlusionSize - 1)/occlusionSize;
  const ivec2 coord          = ivec2(gl_FragCoord.xy)/occlusionRatio;
  const uint  occlusion      = imageLoad(occlusionLut, coord).r;
  [[dont_unroll]]
  for(int i=0; i<steps; i++) {
    bool  bit    = bitfieldExtract(occlusion,i,1)!=0;
    float shadow = bit ? 1.0 : 0.0;

    {
      // scan for consecutive range of same bits
      uint oc = occlusion;
      if(bit)
        oc = oc ^ 0xFFFFFFFF;
      oc = oc & (0xFFFFFFFF << i);
      int lsb = findLSB(oc);
      i = (lsb<0) ? 31 : lsb-1;
    }

    const float t   = (i+0.5)/float(steps);
    const float dd  = (t*distZ)/(dist);

    const vec4  val = textureLod(fogLut, vec3(uv,dd), 0) * min(dd*steps*2.0, 1.0);
    scatteredLight += (val.rgb-prevVal.rgb)*shadow;
    prevVal = val;
#if defined(FOG_SKY_VIS)
    // the multiple scattering of the step, lit as the single one: in the sun's shadow the air is taken as closed to
    // the sky too (Д1); how far that holds, the sky seen from the camera says (below)
    const vec4  msv = textureLod(fogLutMs, vec3(uv,dd), 0) * min(dd*steps*2.0, 1.0);
    msShadowed += (msv.rgb-prevMs.rgb)*shadow;
    prevMs = msv;
#endif
    }
  // A slice of the LUT holds the light scattered up to its far end: the first one, read at any depth nearer than
  // its middle, puts the scattering of the whole slice (some 780 m of air at 50x) on a pixel a metre away, a veil of
  // 0.002 over every daytime pixel (step-2-shade.md, «Привыкание глаза: сделано»). The multiple scattering and the
  // transmittance fade in from the camera as the single scattering above does.
  const float d             = (distZ)/(dist);
  const float near          = min(d*float(textureSize(fogLutMs,0).z)*2.0, 1.0);
  const vec4  ms            = textureLod(fogLutMs, vec3(uv,d), 0);
  const float transmittance = mix(1.0, ms.a, near);
#if defined(FOG_SKY_VIS)
  // Д4 (step-2-shade.md, «Дымка в тени»): each step's multiple scattering by mix(its sun bit, 1, the sky seen from the
  // camera): under the crowns and in a cave the air in the sun's shadow is closed to the sky as well, in the open it
  // is not; 1 gives the fog as it is without this
  const float vis = scene.fogSkyVis;
  return vec4(scatteredLight + ms.rgb*near*vis + msShadowed*(1.0-vis), transmittance);
#else
  return vec4(scatteredLight+ms.rgb*near, transmittance);
#endif
#endif
  }
#else
vec4 fog(vec2 uv, float z) {
  float dZ   = linearDepth(      z, scene.clipInfo);
  float d0   = linearDepth(dFogMin, scene.clipInfo);
  float d1   = linearDepth(dFogMax, scene.clipInfo);
  float d    = (dZ-d0)/(d1-d0);
  // return vec4(debugColors[min(int(d*textureSize(fogLut,0).z), textureSize(fogLut,0).z-1)%MAX_DEBUG_COLORS], 0);

  // fades in from the camera over half the first slice, as the volumetric fog
  const float near = min(d*float(textureSize(fogLut,0).z)*2.0, 1.0);
  const vec4  val  = textureLod(fogLut, vec3(uv,d), 0);
  return vec4(val.rgb*near, mix(1.0, val.a, near));
  }
#endif

#if !defined(GL_COMPUTE_SHADER)
void main_frag() {
  const ivec2 size = textureSize(depth,0);
  inPos = vec2(gl_FragCoord.xy)/vec2(size);
  inPos = inPos*2.0 - vec2(1.0);

  vec2 uv     = vec2(gl_FragCoord.xy)/vec2(size);
  vec3 view   = normalize(inverse(vec3(inPos,1.0)));
  vec3 sunDir = scene.sunDir;

  const float dMax   = texelFetch(depth,ivec2(gl_FragCoord.xy),0).r;
  const bool  isSky  = (dMax==1.0);

  //const vec3  maxVal = isSky ? textureLod(fogLut, vec3(uv, textureSize(fogLut,0).z-1), 0).rgb : vec3(0);
  const vec4  val    = fog(uv,dMax);

  vec3  lum = val.rgb;// - maxVal;
  float tr  = isSky ? 1 : val.a;

  lum *= scene.GSunIntensity;
  lum *= scene.exposure;

  outColor = vec4(lum, 1.0-tr);
  }
#else
void main_comp() {
  const ivec2 size = imageSize(occlusionLut).xy;
  if(invocationID.x>=size.x || invocationID.y>=size.y)
    return;

  // trace from the top-left depth pixel of the texel's block: the block centre lies on a pixel border
  const ivec2 depthSize = textureSize(depth,0);
  const ivec2 depthPix  = min(ivec2(invocationID.xy)*((depthSize + size - 1)/size), depthSize-1);
  inPos = vec2(depthPix+vec2(0.5))/vec2(depthSize);
  inPos = inPos*2.0 - vec2(1.0);

  vec2  uv     = inPos*vec2(0.5)+vec2(0.5);
  vec3  view   = normalize(inverse(vec3(inPos,1.0)));
  vec3  sunDir = scene.sunDir;
  float z      = min(texelFetch(depth,depthPix,0).r, dFogMax);

  fog(uv,z);
  }
#endif

void main() {
#if defined(GL_COMPUTE_SHADER)
  main_comp();
#else
  main_frag();
#endif
  }

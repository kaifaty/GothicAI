#ifndef SCENE_GLSL
#define SCENE_GLSL

// std140, because uniform buffer
struct SceneDesc {
  mat4  viewProject;
  mat4  viewProjectInv;
  mat4  viewShadow[2];
  mat4  viewProjectLwcInv;
  mat4  viewShadowLwc[2];
  mat4  viewVirtualShadow;
  mat4  viewVirtualShadowLwc;
  mat4  viewProject2VirtualShadow;
  vec4  vsmDdx, vsmDdy;
  mat4  view;
  mat4  project;
  mat4  projectInv;
  vec3  sunDir;
  float waveAnim;
  vec3  ambient;
  float exposure;
  vec3  sunColor;
  float GSunIntensity;
  vec4  frustrum[6];
  vec3  clipInfo;
  uint  tickCount32;
  vec3  camPos;
  float isNight;
  vec2  screenResInv;
  vec2  closeupShadowSlice;
  vec3  pfxLeft;
  uint  underWater;
  vec3  pfxTop;
  float luminanceMed; // for debugging
  vec3  pfxDepth;
  float plPosY;
  ivec2 hiZTileSize;
  ivec2 screenRes;
  vec4  cloudsDir;
  float probeLodScale; // the lod of the GI probes by the distance to the camera: probeGridLod (probe_common.glsl)
  uint  fogQuality; // 0 - none, 1 - fog LUT, 2 - separate single and multi-scattering LUTs
  float mipBias;     // TAA: material textures of the main view a little sharper (0 without TAA and in shadows)
  float mipGradScale; // exp2(mipBias), for textureGrad
  uint  taaFrame;  // TAA: the frame number (hashed alpha in main.frag, the lamp taps in light.frag); 0 without TAA
  uint  taaActive; // 1 when the lamp shadows turn their taps by the TAA frame (OPENGOTHIC_VSM_LAMP_TAA), else 0
  uint  motionVectors; // motion vectors of objects into the G-buffer (main view only), 0 elsewhere
  float reactiveLrefInv; // TAA reactive mask: 1/Lref, the added light on screen of a fully reactive additive pixel
  mat4  reprojectClip;   // this frame's clip -> the previous camera's, for a point that stood still (Taa::reprojection)
  mat4  viewProjectPrev; // the previous camera with this frame's jitter: only on vec4(d,0)
  float vsmLampNear;     // cm: a lamp's fixture this close to its light casts no shadow ([INTERNAL] vsmLampNearCm)
  uint  vsmLampSides;    // VSM lamps: a cube face keeps the far side (1), both of the world mesh (2), of objects too (3)
  uint  vsmStudy;        // bench `vsm study <bits>`: VSM_STUDY_* below (0 in the game)
  uint  viewFlags;       // VF_* below: the occluder pass, TAA, shadow views
  float fogSkyVis;       // the sky seen from the camera under GI, 0..1 (fog_sky_vis.comp); 1 without it
  float lampNearKnee;    // a lamp's light never passes this many times its light at the range (light.frag); 0 off
  };

// bench `vsm study <bits>`: the shadow test on a mesh's facets (reports/render-features/2026-11-11-facet-shards)
const uint VSM_STUDY_SUN_PIXEL  = 1u; // the sun: the facet's plane over one pixel, as before 2026-11-11 (direct_light.frag)
const uint VSM_STUDY_LAMP_FACET = 2u; // lamps: the facet's plane also where it stands edge-on to the lamp (light.frag)
const uint VSM_STUDY_CLASS      = 4u; // facets by their cosine to the light and the shadow's answer, as colours
const uint VSM_STUDY_SUN_DIFF   = 8u; // the sun: where the plane over a texel's footprint changes the shadow, as colours

struct LightSource {
  vec3  pos;
  float range;
  vec3  color;
  float cookie; // 0: none; else the light mask's slot (integer part) and the light's yaw in turns (fraction)
  float sourceSize; // cm: the flame's size, which sets the soft edge of its shadow (VSM, light.frag)
  float nearLimit;  // the lamp's own limit of the light near it in place of scene.lampNearKnee; 0: the common one
  float padding1;
  float padding2;
  };

struct MorphDesc {
  uint  indexOffset;
  uint  sample0;
  uint  sample1;
  uint  alpha16_intensity16;
  };

struct Instance {
  mat4x3 mat;
  float  fatness;
  uint   animPtr;
  uint   deformPtr;
  uint   deformCount;
  };

struct IndirectCmd {
  uint  vertexCount;
  uint  instanceCount;
  uint  firstVertex;
  uint  firstInstance;
  uint  writeOffset;
  };

struct Cluster {
  vec4  sphere;
  uint  bucketId_commandId;
  uint  firstMeshlet;
  int   meshletCount;
  uint  instanceId;
  };

const uint VF_HIZ = 0x1; // the occluder pass (V_HiZ): it draws into the main view's depth ahead of the G-buffer
const uint VF_TAA = 0x2; // TAA resolves the frame (also with MetalFX temporal)
const uint VF_SHADOW = 0x8; // a shadow view: the sun's cascades and the virtual shadow map
const uint VF_HAIR_DEBUG = 0x4; // hair pixels show the strand's direction as a colour instead of light (OPENGOTHIC_HAIR_DEBUG=1)

const uint BK_SOLID = 0x1;
const uint BK_SKIN  = 0x2;
const uint BK_MORPH = 0x4;
const uint BK_WATER = 0x8;
const uint BK_NORMAL = 0x10;
const uint BK_ORM    = 0x20;
const uint BK_MATERIAL_MAPS = 0x40;
const uint BK_ANTITILE = 0x80;
const uint BK_ANTITILE_ROTATE = 0x100;
const uint BK_HASHED_ALPHA = 0x200; // our glTF MASK: the alpha test takes a threshold per pixel and TAA frame (main.frag)
const uint BK_HAIR = 0x400; // hair cards: the strand's direction and the hair bit go into the G-buffer (main.frag)
const uint BK_STRAND = 0x800; // strands: the vertex shader makes ribbons of them (strandRibbon, vertex_process.glsl)
const uint BK_EYE = 0x1000; // an eyeball: the lid's shade from the face rig, the eye bit into the G-buffer (main.frag)

// Strands in a shadow view: the first part of the list casts the shadow for all of it (the meshlets are dealt out
// evenly, groommesh.cpp), each strand as wide as that part is small (strandShadowRibbon, vertex_process.glsl).
// count: the meshlets the main view draws now; wider: how much of the list that leaves out (Instance::fatness of
// a strand object: the whole list is count*wider).
const float StrandShadowList = 0.125;
int strandShadowMeshlets(int count, float wider) {
  return min(count, max(1, int(ceil(float(count)*max(wider,1.0)*StrandShadowList))));
  }

struct Bucket {
  vec4  bbox[2];
  ivec2 texAniMapDirPeriod;
  float bboxRadius;
  float waveMaxAmplitude;
  float alphaWeight;
  float envMapping;
  uint  flags;
  float uvScale;
  float antitileCell;
  uint  diffuseFrames;
  uint  frameCount;
  uint  frameDuration;
  float turbidity;
  float turbidityDepth;
  float emissive;
  float reactive; // the TAA reactive weight of the material: 1 with a texture animation, else 0 (graphics plan 1v)
  };

#endif

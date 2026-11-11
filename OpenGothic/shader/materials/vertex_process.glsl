#ifndef VERTEX_PROCESS_GLSL
#define VERTEX_PROCESS_GLSL

#include "common.glsl"

struct Vertex {
#if (MESH_TYPE==T_LANDSCAPE) || (MESH_TYPE==T_OBJ) || (MESH_TYPE==T_MORPH)
  vec3  pos;
  uint  color;
  vec3  normal;
  vec2  uv;
#elif(MESH_TYPE==T_SKINING)
  vec3  pos0;
  vec3  pos1;
  vec3  pos2;
  vec3  pos3;
  uvec4 boneId;
  vec4  weight;
  vec3  normal;
  vec2  uv;
  uint  color;       // not a colour: 0 when normalLocal is valid (Resources::VertexA)
  uvec4 normalLocal; // the normal in each bone's rest frame, octahedral snorm16x2
#elif (MESH_TYPE==T_PFX)
  vec3  pos;
  vec3  normal;
  vec2  uv;
  vec3  size;
  uint  bits0;
  vec3  dir;
  uint  color;
#endif
  };

#if (MESH_TYPE==T_SKINING)
// The octahedral snorm16x2 of packOctNormal (packedmesh.cpp)
vec3 octNormal(uint bits) {
  const vec2 e = unpackSnorm2x16(bits);
  vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
  if(n.z<0.0)
    n.xy = (1.0 - abs(n.yx)) * vec2(n.x>=0.0 ? 1.0 : -1.0, n.y>=0.0 ? 1.0 : -1.0);
  return normalize(n);
  }
#endif

Vertex pullVertex(uint bucketId, uint id) {
#if defined(BINDLESS)
  nonuniformEXT uint vi = bucketId;
#else
  const         uint vi = 0;
#endif
  Vertex ret;
#if (MESH_TYPE==T_SKINING)
  id *=27;
  ret.normal = vec3(vbo[vi].vertices[id + 0], vbo[vi].vertices[id + 1], vbo[vi].vertices[id + 2]);
  ret.uv     = vec2(vbo[vi].vertices[id + 3], vbo[vi].vertices[id + 4]);
  ret.color  = floatBitsToUint(vbo[vi].vertices[id + 5]);
  ret.pos0   = vec3(vbo[vi].vertices[id +  6], vbo[vi].vertices[id +  7], vbo[vi].vertices[id +  8]);
  ret.pos1   = vec3(vbo[vi].vertices[id +  9], vbo[vi].vertices[id + 10], vbo[vi].vertices[id + 11]);
  ret.pos2   = vec3(vbo[vi].vertices[id + 12], vbo[vi].vertices[id + 13], vbo[vi].vertices[id + 14]);
  ret.pos3   = vec3(vbo[vi].vertices[id + 15], vbo[vi].vertices[id + 16], vbo[vi].vertices[id + 17]);
  ret.boneId = uvec4(unpackUnorm4x8(floatBitsToUint(vbo[vi].vertices[id + 18]))*255.0);
  ret.weight = vec4(vbo[vi].vertices[id + 19], vbo[vi].vertices[id + 20], vbo[vi].vertices[id + 21], vbo[vi].vertices[id + 22]);
  ret.normalLocal = uvec4(floatBitsToUint(vbo[vi].vertices[id + 23]), floatBitsToUint(vbo[vi].vertices[id + 24]),
                          floatBitsToUint(vbo[vi].vertices[id + 25]), floatBitsToUint(vbo[vi].vertices[id + 26]));
#elif (MESH_TYPE==T_LANDSCAPE) || (MESH_TYPE==T_OBJ) || (MESH_TYPE==T_MORPH)
  id *= 9;
  ret.pos    = vec3(vbo[vi].vertices[id + 0], vbo[vi].vertices[id + 1], vbo[vi].vertices[id + 2]);
  ret.normal = vec3(vbo[vi].vertices[id + 3], vbo[vi].vertices[id + 4], vbo[vi].vertices[id + 5]);
  ret.uv     = vec2(vbo[vi].vertices[id + 6], vbo[vi].vertices[id + 7]);
  ret.color  = floatBitsToUint(vbo[vi].vertices[id + 8]);
#else
#error "unknown mesh type"
#endif
  return ret;
  }

#if (MESH_TYPE==T_MORPH)
vec3 morphOffset(uint bucketId, uint animPtr, uint vertexIndex) {
#if defined(BINDLESS)
  nonuniformEXT uint i = bucketId;
#else
  const         uint i = 0;
#endif
  MorphDesc md        = pullMorphDesc(animPtr);
  vec2      ai        = unpackUnorm2x16(md.alpha16_intensity16);
  float     alpha     = ai.x;
  float     intensity = ai.y;
  if(intensity<=0)
    return vec3(0);

  uint  vId   = vertexIndex + md.indexOffset;
  int   index = morphId[i].index[vId];
  if(index<0)
    return vec3(0);

  uint  f0 = md.sample0;
  uint  f1 = md.sample1;
  vec3  a  = morph[i].samples[f0 + index].xyz;
  vec3  b  = morph[i].samples[f1 + index].xyz;

  return mix(a,b,alpha) * intensity;
  }
#endif

#if (MESH_TYPE==T_MORPH) && defined(MAT_MOTION)
// morphOffset with the previous frame's descriptor at the same pointer
vec3 morphOffsetPrev(uint bucketId, uint animPtr, uint vertexIndex) {
#if defined(BINDLESS)
  nonuniformEXT uint i = bucketId;
#else
  const         uint i = 0;
#endif
  MorphDesc md        = pullMorphDescPrev(animPtr);
  vec2      ai        = unpackUnorm2x16(md.alpha16_intensity16);
  float     alpha     = ai.x;
  float     intensity = ai.y;
  if(intensity<=0)
    return vec3(0);

  uint  vId   = vertexIndex + md.indexOffset;
  int   index = morphId[i].index[vId];
  if(index<0)
    return vec3(0);

  vec3  a  = morph[i].samples[md.sample0 + index].xyz;
  vec3  b  = morph[i].samples[md.sample1 + index].xyz;
  return mix(a,b,alpha) * intensity;
  }
#endif

#if (MESH_TYPE==T_OBJ)
// Strands as ribbons (graphics/mesh/groommesh.cpp, reports/content/2026-11-npc-hair/strands.md). A point of a strand
// comes as two vertices at one place; here they are moved apart across the strand, at a right angle to the way to
// the eye of this view, so the ribbon always faces it. A hair is thinner than a pixel: the ribbon is drawn a pixel
// wide and takes the hair's share of it as its cover, which the hashed alpha test of main.frag cuts and TAA
// averages. The varyings of such a vertex are its own:
//   uv.x    -1 and 1 at the edges of the ribbon: the strand's direction on the screen is where it does not change
//   uv.y    the strand's tint, 0..255, plus 0.98 of the cover
//   normal  the normal of the hair's surface, as long as 0.5 + half of the light that reaches the point
struct StrandVertex {
  float halfWidth; // cm, 0: not a strand
  float side;
  float id;        // the strand's random number, 0..1
  float tint;
  vec3  tangent;
  };
StrandVertex strandVx;

const float StrandShadowShare = 0.10; // cm: a strand of this width always casts a shadow, a thinner one as often as it is thinner
const float StrandShadowWidth = 0.20; // cm: the ribbon it casts the shadow with. Twice the share: the holes between the few close

vec3 strandOctNormal(vec2 e) {
  e = e*2.0 - 1.0;
  vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
  if(n.z<0.0)
    n.xy = (1.0 - abs(n.yx)) * vec2(n.x>=0.0 ? 1.0 : -1.0, n.y>=0.0 ? 1.0 : -1.0);
  return normalize(n);
  }

// toEye: from the point to the eye of the view, a unit vector. width: what is drawn, cover: the hair's share of it
void strandRibbon(inout Varyings shOut, inout vec3 pos, vec3 toEye, float width, float cover) {
  vec3  s = cross(strandVx.tangent, toEye);
  float l = length(s);
  if(l>1e-4)
    pos += s*(strandVx.side*0.5*width/l);
#if defined(MAT_UV)
  shOut.uv  = vec2(strandVx.side, strandVx.tint + 0.98*cover);
#endif
#if defined(MAT_POSITION)
  shOut.pos = pos;
#endif
  }

// A shadow view: a hair is far thinner than a texel, so a share of the strands, picked by their random numbers,
// stands for all of them, each as wide as that share is small. false: this vertex takes no part
bool strandShadowRibbon(inout Varyings shOut, inout vec3 pos, vec3 toEye) {
  const float width = 2.0*strandVx.halfWidth;
  if(strandVx.id*StrandShadowShare>width)
    return false;
  strandRibbon(shOut, pos, toEye, max(width,StrandShadowWidth), 1.0);
  return true;
  }
#endif

#if (MESH_TYPE==T_PFX)
void rotate(out vec3 rx, out vec3 ry, float a, in vec3 x, in vec3 y){
  const float c = cos(a);
  const float s = sin(a);

  rx.x = x.x*c - y.x*s;
  rx.y = x.y*c - y.y*s;
  rx.z = x.z*c - y.z*s;

  ry.x = x.x*s + y.x*c;
  ry.y = x.y*s + y.y*c;
  ry.z = x.z*s + y.z*c;
  }
#endif

vec3 processVertexCommon(out Varyings shOut, in Vertex v, uint bucketId, uint instanceId, uint vboOffset) {
#if defined(LVL_OBJECT)
  Instance obj = pullInstance(instanceId);
#endif

#if (MESH_TYPE==T_MORPH)
  vec3 morphRestPosition = v.pos;
#endif
#if defined(MAT_MOTION) && (MESH_TYPE==T_OBJ || MESH_TYPE==T_MORPH)
  const vec3 restPos = v.pos; // the previous position below starts from the mesh again
#endif

  // Position offsets
#if (MESH_TYPE==T_MORPH)
  for(int i=0; i<MAX_MORPH_LAYERS; ++i)
    v.pos += morphOffset(bucketId, obj.animPtr+i, vboOffset);
#endif

#if (MESH_TYPE==T_OBJ || MESH_TYPE==T_MORPH) && defined(LVL_OBJECT)
  // Cloth shares the normal object pipeline in every render pass, including
  // Metal/MoltenVK. UVs and colour stay in the immutable packed mesh.
  if(vboOffset<obj.deformCount) {
    uint at = obj.deformPtr + vboOffset*8;
#if (MESH_TYPE==T_MORPH)
    vec3 morphDelta = v.pos-morphRestPosition;
    float simulationWeight = uintBitsToFloat(instanceMem[at+3]);
#endif
    v.pos = vec3(uintBitsToFloat(instanceMem[at+0]),
                 uintBitsToFloat(instanceMem[at+1]),
                 uintBitsToFloat(instanceMem[at+2]));
#if (MESH_TYPE==T_MORPH)
    // Keep the original morph phase in the distance blend. It continues to
    // play while Jolt owns the nearby cloth, so entering/leaving never resets it.
    v.pos += morphDelta*(1.0-simulationWeight);
#endif
    v.normal = vec3(uintBitsToFloat(instanceMem[at+4]),
                    uintBitsToFloat(instanceMem[at+5]),
                    uintBitsToFloat(instanceMem[at+6]));
    }
#endif

  // Normals
  vec3 normal = v.normal;
#if defined(LVL_OBJECT)
  normal = obj.mat*vec4(normal,0);
#endif

  // Position
#if (MESH_TYPE==T_SKINING)
  vec3 pos  = vec3(0);
  vec3 dpos = normal*obj.fatness;
  {
    const uvec4 boneId = v.boneId + uvec4(obj.animPtr);

    const vec3  t0 = (pullMatrix(boneId.x)*vec4(v.pos0,1.0)).xyz;
    const vec3  t1 = (pullMatrix(boneId.y)*vec4(v.pos1,1.0)).xyz;
    const vec3  t2 = (pullMatrix(boneId.z)*vec4(v.pos2,1.0)).xyz;
    const vec3  t3 = (pullMatrix(boneId.w)*vec4(v.pos3,1.0)).xyz;

    pos = (t0*v.weight.x + t1*v.weight.y + t2*v.weight.z + t3*v.weight.w) + dpos;

#if defined(MAT_NORMAL)
    // Shading follows the bones like the position. The fatness offset above keeps the rest normal, as the shadow
    // passes do, so the surface stays where they put it.
    if(v.color==0u) {
      const vec3 n = mat3(pullMatrix(boneId.x))*octNormal(v.normalLocal.x)*v.weight.x +
                     mat3(pullMatrix(boneId.y))*octNormal(v.normalLocal.y)*v.weight.y +
                     mat3(pullMatrix(boneId.z))*octNormal(v.normalLocal.z)*v.weight.z +
                     mat3(pullMatrix(boneId.w))*octNormal(v.normalLocal.w)*v.weight.w;
      if(dot(n,n)>1e-12)
        normal = normalize(n);
      }
#endif
  }
#elif (MESH_TYPE==T_OBJ || MESH_TYPE==T_MORPH)
  vec3 dpos = normal*obj.fatness;
  vec3 pos  = obj.mat*vec4(v.pos,1.0) + dpos;
#else
  vec3 pos  = v.pos;
#endif

#if defined(MAT_UV)
  shOut.uv     = v.uv;
#endif

#if defined(MAT_NORMAL)
  shOut.normal = normal;
#endif

#if defined(MAT_POSITION)
  shOut.pos    = pos;
#endif

#if defined(MAT_COLOR)
  shOut.color = unpackUnorm4x8(v.color);
#endif

#if (MESH_TYPE==T_OBJ)
  strandVx.halfWidth = 0.0;
  if((bucket[bucketId].flags & BK_STRAND)!=0u) {
    // the vertex's normal is the strand's direction; its colour holds the surface normal, the light and the tint
    const vec4  c     = unpackUnorm4x8(v.color);
    const float scale = length(normal);
    strandVx.halfWidth = abs(v.uv.x)*scale;
    strandVx.side      = v.uv.x<0.0 ? -1.0 : 1.0;
    strandVx.id        = v.uv.y;
    strandVx.tint      = floor(c.w*255.0 + 0.5);
    strandVx.tangent   = normal/max(scale,1e-6);
#if defined(MAT_NORMAL)
    shOut.normal = normalize(obj.mat*vec4(strandOctNormal(c.xy),0.0))*(0.5 + 0.5*c.z);
#endif
    }
#endif

#if defined(MAT_MOTION) && defined(LVL_OBJECT)
  // Motion vectors (graphics plan 1b): the same vertex once more, from the previous frame's instance buffer and at the
  // current pointers: bones, matrix (wind, physics), morph descriptors and cloth vertices. The fatness offset is the
  // current one; no second normal, UV or colour. The offset d = previous - current is taken as a difference of the
  // inputs, not of two world positions: it is exactly 0 for what stood still and keeps its precision far from the
  // world's origin. It waits in prevDelta for processVertex
  shOut.prevDelta = vec3(0);
  if(scene.motionVectors!=0u) {
#if (MESH_TYPE==T_SKINING)
    const uvec4 boneId = v.boneId + uvec4(obj.animPtr);
    shOut.prevDelta = ((pullMatrixPrev(boneId.x)-pullMatrix(boneId.x))*vec4(v.pos0,1.0)).xyz*v.weight.x +
                      ((pullMatrixPrev(boneId.y)-pullMatrix(boneId.y))*vec4(v.pos1,1.0)).xyz*v.weight.y +
                      ((pullMatrixPrev(boneId.z)-pullMatrix(boneId.z))*vec4(v.pos2,1.0)).xyz*v.weight.z +
                      ((pullMatrixPrev(boneId.w)-pullMatrix(boneId.w))*vec4(v.pos3,1.0)).xyz*v.weight.w;
#else
    vec3 local = restPos;
#if (MESH_TYPE==T_MORPH)
    for(int i=0; i<MAX_MORPH_LAYERS; ++i)
      local += morphOffsetPrev(bucketId, obj.animPtr+i, vboOffset);
#endif
    if(vboOffset<obj.deformCount) {
      uint at = obj.deformPtr + vboOffset*8;
#if (MESH_TYPE==T_MORPH)
      vec3  morphDelta       = local-restPos;
      float simulationWeight = uintBitsToFloat(instanceMemPrev[at+3]);
#endif
      local = vec3(uintBitsToFloat(instanceMemPrev[at+0]),
                   uintBitsToFloat(instanceMemPrev[at+1]),
                   uintBitsToFloat(instanceMemPrev[at+2]));
#if (MESH_TYPE==T_MORPH)
      local += morphDelta*(1.0-simulationWeight);
#endif
      }
    // prev.mat*local' - mat*local = (prev.mat - mat)*local' + mat*(local' - local); v.pos is this frame's local
    const mat4x3 prevMat = pullInstanceMatPrev(instanceId);
    shOut.prevDelta = (prevMat - obj.mat)*vec4(local,1.0) + obj.mat*vec4(local - v.pos,0.0);
#endif
    }
#endif

  return pos;
  // return scene.viewProject*vec4(pos,1.0);
  }

vec4 processVertex(out Varyings shOut, in Vertex v, uint bucketId, uint instanceId, uint vboOffset) {
  vec3 pos  = processVertexCommon(shOut, v, bucketId, instanceId, vboOffset);
#if (MESH_TYPE==T_OBJ)
  if(strandVx.halfWidth>0.0) {
    if((scene.viewFlags & VF_SHADOW)!=0u) {
      if(!strandShadowRibbon(shOut, pos, scene.sunDir))
        return vec4(uintBitsToFloat(0x7fc00000));
      }
    else if((scene.viewFlags & VF_HIZ)!=0u) {
      return vec4(uintBitsToFloat(0x7fc00000)); // a hair hides nothing: never an occluder
      }
    else {
      // a pixel at the point's depth, in the world's units
      const float depth = (scene.viewProject*vec4(pos,1.0)).w;
      const float pixel = 2.0*depth/(abs(scene.project[1][1])*float(scene.screenRes.y));
      const float width = 2.0*strandVx.halfWidth;
      strandRibbon(shOut, pos, normalize(scene.camPos-pos), max(width,pixel), clamp(width/max(pixel,1e-6),0.0,1.0));
      }
    }
#endif
  vec4 clip = scene.viewProject*vec4(pos,1.0);
#if defined(MAT_MOTION)
  // the residual is divided per fragment (main.frag): both are linear in the world position
  if(scene.motionVectors!=0u) {
    shOut.prevStatic = (scene.reprojectClip*clip).xyw;
    shOut.prevDelta  = (scene.viewProjectPrev*vec4(shOut.prevDelta,0.0)).xyw;
    } else {
    shOut.prevStatic = vec3(0,0,1);
    shOut.prevDelta  = vec3(0);
    }
#endif
  return clip;
  }

#endif

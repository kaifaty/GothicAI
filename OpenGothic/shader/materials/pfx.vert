#version 450

#extension GL_ARB_separate_shader_objects : enable
#extension GL_GOOGLE_include_directive : enable

#include "materials_common.glsl"

const int PfxOrientationNone       = 0;
const int PfxOrientationVelocity   = 1;
const int PfxOrientationVelocity3d = 2;
const int PfxOrientationStretch    = 3; // own streak (pfxbucket.cpp, particles.d "stretch")

struct Particle {
  vec3  pos;
  uint  color;
  vec3  size;
  uint  bits0;
  vec3  dir;
  uint  colorB;
  };

layout(binding = L_Pfx, std430) readonly buffer PfxBuf {
  Particle pfx[];
  };

out gl_PerVertex {
  vec4 gl_Position;
  };
#if defined(MAT_VARYINGS)
layout(location = 0) out flat uint bucketId;
layout(location = 1) out Varyings  shOut;
#endif

#if DEBUG_DRAW
layout(location = DEBUG_DRAW_LOC) out flat uint debugId;
#endif

struct Vertex {
  vec3  pos;
  vec3  normal;
  vec2  uv;
  vec3  size;
  uint  bits0;
  vec3  dir;
  uint  color;
  };

#if defined(FORWARD)
vec3 unpackRgb9e5(uint v) {
  const float scale = exp2(float(v >> 27u) - 15.0 - 9.0);
  return vec3(v & 511u, (v >> 9u) & 511u, (v >> 18u) & 511u) * scale;
  }
#endif

Vertex pullVertex(uint id) {
  Vertex ret;
  ret.pos    = pfx[id].pos;
  ret.color  = pfx[id].color;
  ret.size   = pfx[id].size;
  ret.bits0  = pfx[id].bits0;
  ret.dir    = pfx[id].dir;
  ret.uv     = vec2(0);
  ret.normal = vec3(0);
  return ret;
  }

void rotate(out vec3 rx, out vec3 ry, float a, in vec3 x, in vec3 y) {
  const float c = cos(a);
  const float s = sin(a);

  rx.x = x.x*c - y.x*s;
  rx.y = x.y*c - y.y*s;
  rx.z = x.z*c - y.z*s;

  ry.x = x.x*s + y.x*c;
  ry.y = x.y*s + y.y*c;
  ry.z = x.z*s + y.z*c;
  }

vec4 processVertex(out Varyings shOut, in Vertex v, uint instanceId, uint vboOffset) {
  float energy = 1.0; // own least size on screen: the alpha that keeps the sprite's light as it grows
  // Bilboards
  {
    const float U[6]   = { 0, 1, 0,  0, 1, 1};
    const float V[6]   = { 1, 0, 0,  1, 1, 0};

    const float dxQ[6] = {-0.5, 0.5, -0.5, -0.5,  0.5,  0.5};
    const float dyQ[6] = { 0.5,-0.5, -0.5,  0.5,  0.5, -0.5};

    const float dxT[6] = {-0.3333,  1.5, -0.3333, 0,0,0};
    const float dyT[6] = { 1.5, -0.3333, -0.3333, 0,0,0};

    const bool  visZBias         = bitfieldExtract(v.bits0, 0, 1)!=0;
    const bool  visTexIsQuadPoly = bitfieldExtract(v.bits0, 1, 1)!=0;
    const bool  visYawAlign      = bitfieldExtract(v.bits0, 2, 1)!=0;
    const bool  isTrail          = bitfieldExtract(v.bits0, 3, 1)!=0;
    const uint  visOrientation   = bitfieldExtract(v.bits0, 4, 2);

    //const mat4  m      = scene.viewProject;
    const mat4  m      = scene.view;
    vec3        left   = scene.pfxLeft;
    vec3        top    = scene.pfxTop;
    vec3        depth  = scene.pfxDepth;

    if(visYawAlign) {
      left = vec3(left.x, 0, left.z);
      top  = vec3(0, -1, 0);
      }

    v.uv     = vec2(U[vboOffset],V[vboOffset]);
    v.normal = -depth;

    if(isTrail) {
      const uint colorB = pfx[instanceId].colorB;
      v.normal = vec3(0,1,0);

      if(dyQ[vboOffset]>0)
        v.color = colorB;

      float tA     = v.size.y;
      float tB     = v.size.z;

      vec3 n  = cross(depth,v.dir);
      n = normalize(n);
      n = n*v.size.x;

      v.pos += (dxQ[vboOffset])*n + (dyQ[vboOffset]+0.5)*v.dir;

      v.uv.x = dxQ[vboOffset]+0.5;
      v.uv.y = 1.0-(tA + (dyQ[vboOffset]+0.5)*(tB-tA));
      }
    else if(visOrientation==PfxOrientationVelocity3d) {
      float ldir = length(v.dir);
      if(ldir!=0.f)
        v.dir/=ldir;
      top  = -v.dir;
      left = -cross(top,depth);
      }
    else if(visOrientation==PfxOrientationStretch) {
      // a card facing the camera along the velocity as seen on screen: size.y plus the path size.z (cm) foreshortened,
      // its head at the particle and the streak behind it
      const float speed = length(v.dir);
      const vec3  dn    = speed>0.0 ? v.dir/speed : vec3(0);
      const vec3  dp    = dn - dot(dn,depth)*depth;
      const float lp    = length(dp);
      if(lp>1e-4) {
        top  = -dp/lp;
        left = -cross(top,depth);
        }
      v.pos    -= dn*(v.size.z*0.5);
      v.size.y += v.size.z*lp;
      v.size.z  = 0.1*v.size.x;
      }
    else if(visOrientation==PfxOrientationVelocity) {
      vec3  dir  = (m * vec4(v.dir,0)).xyz;
      float ldir = length(dir);
      dir = (ldir>0) ? (dir/ldir) : vec3(0);

      float sVel = 1.5f*(1.f - abs(dir.z));
      float c    = -dir.x;
      float s    =  dir.y;
      float rot  = (s==0 && c==0) ? 0.f : atan(s,c);

      vec3 l,t;
      rotate(l,t,rot+float(M_PI/2),left,top);
      left = l*sVel;
      top  = t*sVel;
      }
    else {
      // nope
      }

    // own least size on screen (pfxbucket.cpp, particles.d "minPixels"; 1/16 px in bits 7..14 without a flipbook):
    // a smaller sprite is drawn that size and its alpha falls by the area, so its light stays and TAA keeps it
    const uint minPx16 = bitfieldExtract(v.bits0, 6, 1)!=0 ? 0u : bitfieldExtract(v.bits0, 7, 8);
    if(minPx16!=0u && !isTrail) {
      const float w     = max((scene.viewProject*vec4(v.pos,1.0)).w, 1e-3);
      const float pxPer = abs(scene.project[1][1])*0.5*float(scene.screenRes.y)/w;
      const vec2  k     = max(vec2(1.0), (float(minPx16)/16.0)/max(v.size.xy*pxPer, vec2(1e-4)));
      v.size.xy *= k;
      energy     = 1.0/(k.x*k.y);
      }

    if(visTexIsQuadPoly)
      v.pos += left*dxQ[vboOffset]*v.size.x + top*dyQ[vboOffset]*v.size.y;
    else if(!isTrail)
      v.pos += left*dxT[vboOffset]*v.size.x + top*dyT[vboOffset]*v.size.y;

    if(visZBias)
      v.pos -= v.size.z*depth;
  }

#if defined(MAT_UV)
  shOut.uv     = v.uv;
#endif
#if defined(MAT_NORMAL)
  shOut.normal = v.normal;
#endif
#if defined(MAT_POSITION)
  shOut.pos    = v.pos;
#endif
#if defined(MAT_COLOR)
  shOut.color = unpackUnorm4x8(v.color);
  shOut.color.a *= energy;
#endif
#if defined(MAT_UV)
  shOut.flip = vec3(0, 0, -1);
  if(bitfieldExtract(v.bits0, 6, 1)!=0) {
    // own atlas flipbook (pfxbucket.cpp): this frame and the next, cols x rows cells from the top left
    const uvec2 grid  = uvec2(bitfieldExtract(v.bits0, 7, 3), bitfieldExtract(v.bits0, 10, 3)) + 1u;
    const uint  f0    = bitfieldExtract(v.bits0, 13, 6);
    const uint  f1    = bitfieldExtract(v.bits0, 19, 6);
    const vec2  cell  = vec2(1.0)/vec2(grid);
    shOut.uv   = (vec2(f0 % grid.x, f0 / grid.x) + v.uv) * cell;
    shOut.flip = vec3((vec2(f1 % grid.x, f1 / grid.x) + v.uv) * cell, float(bitfieldExtract(v.bits0, 25, 7))/127.0);
    }
#endif
#if defined(FORWARD)
  // billboards carry their point-light irradiance in colorB; a trail uses it for its second colour
  shOut.pointLight = bitfieldExtract(v.bits0, 3, 1)!=0 ? vec3(0) : unpackRgb9e5(pfx[instanceId].colorB);
#endif

  return scene.viewProject*vec4(v.pos,1.0);
  }

void main() {
  const Vertex vert = pullVertex(gl_InstanceIndex);

  Varyings var;
  gl_Position = processVertex(var, vert, gl_InstanceIndex, gl_VertexIndex);
#if defined(MAT_VARYINGS)
  bucketId = 0;
  shOut    = var;
#endif
  }

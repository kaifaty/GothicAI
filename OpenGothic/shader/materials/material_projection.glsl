#ifndef MATERIAL_PROJECTION_GLSL
#define MATERIAL_PROJECTION_GLSL

// A material's maps by world position instead of the mesh UV (materials.json "projection", BK_PROJECTION): rock whose
// original unwrap tilts the beds, stretches faces and breaks at seams (reports/content/2026-11-landscape-pilot/
// b3-rock-material.md, task 1). Biplanar mapping (Quilez, https://iquilezles.org/articles/biplanar/): the two planes
// the normal faces most, two fetches per map where they blend, one elsewhere. In a side plane v is the world's height
// (down, as the engine's v), so beds stay horizontal on any face; from above and below the plane is XZ. The bucket's
// uvScale carries repeats per cm (drawbuckets.cpp). Only the G-buffer of opaque world and static meshes: elsewhere off.

// how fast a plane gives way past 45 degrees: 1 is Quilez's k=8; 2 halves the band where two planes show at once
const float ProjectionSharpness = 2.0;
// below this share the second plane is not fetched; the shares are remapped so the cut does not show
const float ProjectionSkip      = 1.0/64.0;
const float ProjectionDiagonal  = 0.57735027; // |n| on the diagonal (1,1,1): the least share a major axis has

struct Projection {
  bool  on;
  float w1;               // the second plane's share, the first's is 1-w1; 0: the second is not fetched
  vec2  uv0, uv1;         // coordinates in the two planes, repeats
  vec2  dx0, dy0, dx1, dy1; // their screen gradients, from the position's (no mip jump where the planes change)
  vec3  t0, b0, t1, b1;   // where u and v grow along the surface in each plane, as mappedNormal's frame
  vec3  n;
  };

#if defined(GBUFFER)
void projectionPlane(out vec2 uv, out vec2 dx, out vec2 dy, out vec3 t, out vec3 b,
                     uint axis, vec3 n, vec3 pos, vec3 dpdx, vec3 dpdy, float k, float frame) {
  const float side = n[axis]>=0.0 ? 1.0 : -1.0;
  // cross(u, v) is the plane's axis toward the surface: the texture is not mirrored on either side
  vec3 eu, ev;
  if(axis==1u) {
    eu = vec3(1,0,0);
    ev = vec3(0,0,-side);
    } else if(axis==0u) {
    eu = vec3(0,0,side);
    ev = vec3(0,-1,0);
    } else {
    eu = vec3(-side,0,0);
    ev = vec3(0,-1,0);
    }
  uv = vec2(dot(pos,eu), dot(pos,ev))*k;
  dx = vec2(dot(dpdx,eu), dot(dpdx,ev))*k*scene.mipGradScale;
  dy = vec2(dot(dpdy,eu), dot(dpdy,ev))*k*scene.mipGradScale;
  // mappedNormal's cotangent frame of this planar mapping: the surface gradients of u and v times the screen
  // determinant. Its sign is kept, so a face lights as the same map does through a mesh UV laid the same way
  t = (eu - n*dot(n,eu))*frame;
  b = (ev - n*dot(n,ev))*frame;
  }
#endif

void projectionPlanes(out Projection p) {
  p.on = false;
#if defined(GBUFFER) && (MESH_TYPE!=T_PFX)
  p.on = (bucket[bucketId].flags & BK_PROJECTION)!=0u;
  const vec3 pos  = shInp.pos;
  const vec3 dpdx = dFdx(pos), dpdy = dFdy(pos);
  const vec3 n    = normalize(shInp.normal);
  const vec3 a    = abs(n);
  const uint major  = (a.x>=a.y && a.x>=a.z) ? 0u : (a.y>=a.z ? 1u : 2u);
  const uint o1     = (major+1u)%3u, o2 = (major+2u)%3u;
  const uint median = a[o1]>=a[o2] ? o1 : o2;
  // Quilez's shares: 0 at the diagonal for the median axis, so the third plane is never needed and the median
  // changes where its share is 0
  vec2 w = clamp((vec2(a[major], a[median]) - ProjectionDiagonal)/(1.0 - ProjectionDiagonal), 0.0, 1.0);
  w = pow(w, vec2(ProjectionSharpness));
  p.w1 = w.y/max(w.x + w.y, 1e-6);
  p.w1 = max(p.w1 - ProjectionSkip, 0.0)/(1.0 - ProjectionSkip);
  p.n  = n;
  const float k     = bucket[bucketId].uvScale;
  const float frame = dot(n, cross(dpdx,dpdy))<0.0 ? -1.0 : 1.0;
  projectionPlane(p.uv0, p.dx0, p.dy0, p.t0, p.b0, major,  n, pos, dpdx, dpdy, k, frame);
  projectionPlane(p.uv1, p.dx1, p.dy1, p.t1, p.b1, median, n, pos, dpdx, dpdy, k, frame);
#endif
  }

// called only where p.on, which projectionPlanes sets in the G-buffer alone
vec4 projectionAlbedo(in Projection p, uint tId) {
#if defined(BINDLESS)
  nonuniformEXT uint texId = tId;
#else
  const uint texId = 0;
#endif
  vec4 c = textureGrad(sampler2D(textureMain[texId], samplerMain), p.uv0, p.dx0, p.dy0);
  if(p.w1>0.0)
    c = mix(c, textureGrad(sampler2D(textureMain[texId], samplerMain), p.uv1, p.dx1, p.dy1), p.w1);
  return c;
  }

#if defined(GBUFFER) && (MESH_TYPE!=T_PFX)
vec3 projectionNormal(vec3 t, vec3 b, vec3 n, vec4 tex) {
  const vec2  xy   = tex.ag * 2.0 - 1.0; // DXT5nm, as mappedNormal
  const float len2 = max(dot(t,t), dot(b,b));
  if(len2<=1e-20)
    return n;
  const float s = inversesqrt(len2);
  return normalize(t*(xy.x*s) + b*(xy.y*s) + n*sqrt(max(0.0, 1.0 - dot(xy,xy))));
  }

// Each plane turns its own normal (one tangent frame for a projected map is wrong), then they blend
void projectionMaps(in Projection p, uint tId, inout uint normal, inout vec3 orm) {
#if defined(BINDLESS)
  nonuniformEXT uint texId = tId;
#else
  const uint texId = 0;
#endif
  if((bucket[bucketId].flags & BK_NORMAL)!=0u) {
    vec3 nm = projectionNormal(p.t0, p.b0, p.n,
                               textureGrad(sampler2D(textureNormal[texId], samplerMain), p.uv0, p.dx0, p.dy0));
    if(p.w1>0.0)
      nm = mix(nm, projectionNormal(p.t1, p.b1, p.n,
                                    textureGrad(sampler2D(textureNormal[texId], samplerMain), p.uv1, p.dx1, p.dy1)), p.w1);
    normal = encodeNormal(normalize(nm));
    }
  if((bucket[bucketId].flags & BK_ORM)!=0u) {
    orm = textureGrad(sampler2D(textureOrm[texId], samplerMain), p.uv0, p.dx0, p.dy0).rgb;
    if(p.w1>0.0)
      orm = mix(orm, textureGrad(sampler2D(textureOrm[texId], samplerMain), p.uv1, p.dx1, p.dy1).rgb, p.w1);
    }
  }
#endif

#endif

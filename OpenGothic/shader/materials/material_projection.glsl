#ifndef MATERIAL_PROJECTION_GLSL
#define MATERIAL_PROJECTION_GLSL

// A material's maps by world position instead of the mesh UV (materials.json "projection", BK_PROJECTION): rock whose
// original unwrap tilts the beds, stretches faces and breaks at seams (reports/content/2026-11-landscape-pilot/
// b3-rock-material.md, task 1). Biplanar mapping (Quilez, https://iquilezles.org/articles/biplanar/): the two planes
// the selecting normal faces most, two fetches per map where they blend, one elsewhere. In a side plane v is the
// world's height (down, as the engine's v), so beds stay horizontal on any face; from above and below the plane is XZ.
// The bucket's uvScale carries repeats per cm (drawbuckets.cpp). Only the G-buffer of opaque world and static meshes.

// how fast a plane gives way past 45 degrees: 1 is Quilez's k=8; 2 halves the band where two planes show at once
const float ProjectionSharpness = 2.0;
// below this share a plane is not fetched; both shares are remapped alike, so the cut does not show and an even
// split stays even where the major axis changes
const float ProjectionSkip      = 1.0/64.0;
const float ProjectionDiagonal  = 0.57735027; // |n| on the diagonal (1,1,1): the least share a major axis has
// how far the normal that picks the planes may stand off the face: sin and cos of 20 degrees
const float ProjectionLimitSin  = 0.34202014;
const float ProjectionLimitCos  = 0.93969262;

// The fringe (materials.json "fringe", BK_FRINGE; task 2 of the note): grass over the projected rock along the edges
// of the strips the original laid with its v from 0 at the top edge of a wall to 1 at its foot, where its artist
// painted grass. Solid from an edge to the first share of v, tufts to the second: there a pixel is grass where the
// layer's height (the blue of its ORM; grass has no metal) stands above a threshold rising from 0 to 1 across the
// zone. The zone's depth wanders along the wall by world position, so its lower edge is no line from afar either,
// where the height is averaged by the mips. A pixel is grass or rock: colour, normal and ORM all come from one.
const float FringeWander   = 0.30;  // the shares of a band change by up to this part of themselves...
const float FringeWanderCm = 900.0; // ...over this length of the world, plus half as much over a quarter of it
const float FringeDetail   = 0.15;

struct Projection {
  bool  on;
  float w1;               // the second plane's share, the first's is 1-w1; 0: the second is not fetched
  vec2  uv0, uv1;         // coordinates in the two planes, repeats
  vec2  dx0, dy0, dx1, dy1; // their screen gradients, from the position's (no mip jump where the planes change)
  vec3  t0, b0, t1, b1;   // where u and v grow along the surface in each plane, as mappedNormal's frame
  vec3  n;
  bool  fringe;           // this pixel is the layer's
  float fk;               // the layer's repeats per repeat of the projection
  uint  fTex;             // its maps in the bindless arrays
  vec3  fOrm;             // its ORM where the height was fetched to decide (fOrmSet)
  bool  fOrmSet;
  };

#if defined(GBUFFER) && (MESH_TYPE!=T_PFX) && !defined(BINDLESS)
// without bindless the draw binds the layer's maps here (DrawCommands::setBindings), a fallback without a fringe
const uint L_FringeMain   = 23;
const uint L_FringeNormal = 24;
const uint L_FringeOrm    = 25;
layout(binding = L_FringeMain)   uniform texture2D textureFringeMain;
layout(binding = L_FringeNormal) uniform texture2D textureFringeNormal;
layout(binding = L_FringeOrm)    uniform texture2D textureFringeOrm;
#endif

#if defined(GBUFFER)
void projectionPlane(out vec2 uv, out vec2 dx, out vec2 dy, out vec3 t, out vec3 b,
                     uint axis, vec3 select, vec3 n, vec3 pos, vec3 dpdx, vec3 dpdy, float k, float frame) {
  const float side = select[axis]>=0.0 ? 1.0 : -1.0;
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

#if defined(GBUFFER) && (MESH_TYPE!=T_PFX)
// map: 0 albedo, 1 normal, 2 ORM
vec4 fringeTexture(uint map, uint id, vec2 uv, vec2 dx, vec2 dy) {
#if defined(BINDLESS)
  // after the buckets and their frames in the same arrays (DrawBuckets::fringeMaps)
  nonuniformEXT uint fId = id;
  if(map==0u)
    return textureGrad(sampler2D(textureMain[fId],   samplerMain), uv, dx, dy);
  if(map==1u)
    return textureGrad(sampler2D(textureNormal[fId], samplerMain), uv, dx, dy);
  return textureGrad(sampler2D(textureOrm[fId], samplerMain), uv, dx, dy);
#else
  if(map==0u)
    return textureGrad(sampler2D(textureFringeMain,   samplerMain), uv, dx, dy);
  if(map==1u)
    return textureGrad(sampler2D(textureFringeNormal, samplerMain), uv, dx, dy);
  return textureGrad(sampler2D(textureFringeOrm, samplerMain), uv, dx, dy);
#endif
  }

// the layer in the projection's two planes at its own tile: the plane coordinates are linear in the position
vec4 fringeProjected(in Projection p, uint map, uint plane) {
  if(plane==0u)
    return fringeTexture(map, p.fTex, p.uv0*p.fk, p.dx0*p.fk, p.dy0*p.fk);
  return fringeTexture(map, p.fTex, p.uv1*p.fk, p.dx1*p.fk, p.dy1*p.fk);
  }

vec4 fringeBlend(in Projection p, uint map) {
  vec4 c = fringeProjected(p, map, 0u);
  if(p.w1>0.0)
    c = mix(c, fringeProjected(p, map, 1u), p.w1);
  return c;
  }

float fringeHash(ivec2 c) {
  uint h = uint(c.x)*0x8da6b343u ^ uint(c.y)*0xd8163841u;
  h = (h ^ (h >> 16u))*0x7feb352du;
  h = (h ^ (h >> 15u))*0x846ca68bu;
  return float((h ^ (h >> 16u)) & 0xffffu)*(2.0/65535.0) - 1.0;
  }

// smooth value noise, -1..1
float fringeNoise(vec2 x) {
  const ivec2 c = ivec2(floor(x));
  vec2 f = fract(x);
  f = f*f*(3.0 - 2.0*f);
  return mix(mix(fringeHash(c),            fringeHash(c + ivec2(1,0)), f.x),
             mix(fringeHash(c + ivec2(0,1)), fringeHash(c + ivec2(1,1)), f.x), f.y);
  }

void projectionFringe(inout Projection p) {
  if(!p.on || (bucket[bucketId].flags & BK_FRINGE)==0u)
    return;
  // a strip repeats down its v: each repeat has its own edges
  const float v = fract(shInp.uv.y);
  const vec2  x = shInp.pos.xz/FringeWanderCm;
  const float m = 1.0 + FringeWander*fringeNoise(x) + FringeDetail*fringeNoise(x*4.0 + 17.0);
  const vec2  top = vec2(bucket[bucketId].fringeTop0,    bucket[bucketId].fringeTop1)*m;
  const vec2  bot = vec2(bucket[bucketId].fringeBottom0, bucket[bucketId].fringeBottom1)*m;
  // across the tufts 0..1 from the solid band, below 0 in it
  const float tTop = top.y>0.0 ? (v - top.x)/max(top.y - top.x, 1e-4)       : 2.0;
  const float tBot = bot.y>0.0 ? (1.0 - v - bot.x)/max(bot.y - bot.x, 1e-4) : 2.0;
  const float t    = min(tTop, tBot);
  if(t>=1.0)
    return;
  p.fTex = bucket[bucketId].fringeTex;
  p.fk   = bucket[bucketId].fringeScale;
  if(t<=0.0) {
    p.fringe = true;
    return;
    }
  p.fOrm    = fringeBlend(p, 2u).rgb;
  p.fOrmSet = true;
  p.fringe  = p.fOrm.b>t;
  }
#endif

void projectionPlanes(out Projection p) {
  p.on      = false;
  p.fringe  = false;
  p.fOrmSet = false;
  p.fk      = 1.0;
  p.fTex    = 0u;
#if defined(GBUFFER) && (MESH_TYPE!=T_PFX)
  p.on = (bucket[bucketId].flags & BK_PROJECTION)!=0u;
  const vec3 pos  = shInp.pos;
  const vec3 dpdx = dFdx(pos), dpdy = dFdy(pos);
  const vec3 n    = normalize(shInp.normal);
  // The planes are picked by the vertex normal kept within 20 degrees of the face. The vertex normal alone gives shares
  // continuous across facets, but Gothic smooths it over sharp edges of rock by up to 45 degrees, and a plane picked
  // that far off lies across the face and smears it; the face normal alone keeps the shares constant on a facet and
  // jumps on its edges. Within the limit it is the vertex normal; past it, the face normal turned toward it by the
  // limit: facets up to 40 degrees apart continue without a break, a sharper edge keeps a smaller one
  vec3 face = cross(dpdx,dpdy);
  face = dot(face,face)>1e-20 ? normalize(face) : n;
  if(dot(face,n)<0.0)
    face = -face;
  vec3 select = n;
  if(dot(face,n)<ProjectionLimitCos) {
    const vec3 side = n - face*dot(face,n);
    select = dot(side,side)>1e-12 ? face*ProjectionLimitCos + normalize(side)*ProjectionLimitSin : face;
    }
  const vec3 a    = abs(select);
  const uint major  = (a.x>=a.y && a.x>=a.z) ? 0u : (a.y>=a.z ? 1u : 2u);
  const uint o1     = (major+1u)%3u, o2 = (major+2u)%3u;
  const uint median = a[o1]>=a[o2] ? o1 : o2;
  // Quilez's shares: 0 at the diagonal for the median axis, so the third plane is never needed and the median
  // changes where its share is 0
  vec2 w = clamp((vec2(a[major], a[median]) - ProjectionDiagonal)/(1.0 - ProjectionDiagonal), 0.0, 1.0);
  w = pow(w, vec2(ProjectionSharpness));
  const float r  = w.y/max(w.x + w.y, 1e-6);
  const float s0 = max(1.0 - r - ProjectionSkip, 0.0), s1 = max(r - ProjectionSkip, 0.0);
  p.w1 = s1/(s0 + s1);
  p.n  = n;
  const float k     = bucket[bucketId].uvScale;
  const float frame = dot(n, cross(dpdx,dpdy))<0.0 ? -1.0 : 1.0;
  projectionPlane(p.uv0, p.dx0, p.dy0, p.t0, p.b0, major,  select, n, pos, dpdx, dpdy, k, frame);
  projectionPlane(p.uv1, p.dx1, p.dy1, p.t1, p.b1, median, select, n, pos, dpdx, dpdy, k, frame);
  projectionFringe(p);
#endif
  }

// called only where p.on, which projectionPlanes sets in the G-buffer alone
vec4 projectionAlbedo(in Projection p, uint tId) {
#if defined(GBUFFER) && (MESH_TYPE!=T_PFX)
  if(p.fringe)
    return fringeBlend(p, 0u);
#endif
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
  if(p.fringe) {
    // grass lit as grass: its own relief and roughness, nothing of the rock's under it
    vec3 nm = projectionNormal(p.t0, p.b0, p.n, fringeProjected(p, 1u, 0u));
    if(p.w1>0.0)
      nm = mix(nm, projectionNormal(p.t1, p.b1, p.n, fringeProjected(p, 1u, 1u)), p.w1);
    normal = encodeNormal(normalize(nm));
    orm    = vec3((p.fOrmSet ? p.fOrm : fringeBlend(p, 2u).rgb).rg, 0.0);
    return;
    }
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

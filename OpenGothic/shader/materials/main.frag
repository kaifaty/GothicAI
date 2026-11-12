#version 450

#extension GL_ARB_separate_shader_objects : enable
#extension GL_GOOGLE_include_directive : enable

#if defined(VIRTUAL_SHADOW)
#include "virtual_shadow/vsm_common.glsl"
#endif

#include "materials_common.glsl"
#include "water/gerstner_wave.glsl"
#include "lighting/shadow_sampling.glsl"
#include "lighting/tonemapping.glsl"
#include "lighting/material_brdf.glsl"
#if defined(MAT_SUN_SHADOW)
#include "virtual_shadow/vsm_sun_shadow.glsl"
#endif

#if defined(MAT_VARYINGS)
layout(location = 0) in flat uint bucketId;
layout(location = 1) in Varyings  shInp;
#endif

#if defined(VIRTUAL_SHADOW)
layout(location = 3) in flat uint vsmMipId;
#endif

#if DEBUG_DRAW
layout(location = DEBUG_DRAW_LOC) in flat uint debugId;
#endif

#if defined(GBUFFER)
layout(location = 0) out vec4 outDiffuse;
layout(location = 1) out uint outNormal;
#if defined(MATERIAL_MAPS)
layout(location = 2) out vec4 outOrm;
layout(location = 3) out vec2 outMotion; // Renderer::gbufMotion: after the material maps
#else
layout(location = 2) out vec2 outMotion;
#endif
#elif defined(WATER)
layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outDiffuse;
layout(location = 2) out uint outNormal;
#elif !defined(DEPTH_ONLY)
layout(location = 0) out vec4 outColor;
#endif

// The TAA reactive mask (graphics plan 1v, Renderer::taa.reactive): the second attachment of the translucent pass,
// R8. The pass blends it as the colour: the alpha of this output weighs it (Shaders::materialPipeline)
#if (defined(FORWARD) || (defined(EMISSIVE) && !defined(MULTIPLY)) || defined(GHOST)) && !defined(DEPTH_ONLY) && \
    !defined(GBUFFER) && !defined(WATER)
#define TAA_REACTIVE
layout(location = 1) out vec4 outReactive;
#if (MESH_TYPE==T_PFX) && !defined(GHOST)
layout(push_constant, std430) uniform PushPfx {
  float reactive; // ParticleFx::taaReactive: 1 for particles, 0 for rain and snow
  } pushPfx;
#endif
#endif

#if defined(VIRTUAL_SHADOW)
layout(push_constant, std430) uniform Push {
  uint commandId;
  } push;
#endif

#if defined(WATER) || defined(GHOST)
float unproject(float depth) {
  mat4 projInv = scene.projectInv;
  vec4 o;
  o.z = depth * projInv[2][2] + projInv[3][2];
  o.w = depth * projInv[2][3] + projInv[3][3];
  return o.z/o.w;
  }
#endif

bool isFlat() {
#if defined(GBUFFER) && defined(FLAT_NORMAL)
  {
    vec3 pos   = shInp.pos;
    vec3 dx    = dFdx(pos);
    vec3 dy    = dFdy(pos);
    vec3 flatN = (cross(dx,dy));
    if(dot(normalize(flatN),scene.sunDir)<=0.01)
      return true;
  }
#endif
  return false;
  }

float encodeHintBits() {
  const int flt  = (isFlat() ? 1 : 0) << 1;
#if defined(ATEST)
  const int atst = (1) << 2;
#else
  const int atst = (0) << 2;
#endif

#if defined(WATER)
  const int water = (gl_FrontFacing) ? 0 : (1 << 3);
#elif defined(LVL_OBJECT)
  // const int water = (bucket.envMapping>0.01 ? 1 : 0) << 3;
  const int water = (0) << 3;
#else
  const int water = (0) << 3;
#endif

  return float(flt | atst | water)/255.0;
  }

#if defined(GBUFFER)
vec3 flatNormal() {
#if defined(FLAT_NORMAL)
  vec3 pos   = shInp.pos;
  vec3 dx    = dFdx(pos);
  vec3 dy    = dFdy(pos);
  return normalize(cross(dx,dy));
#else
  return shInp.normal;
#endif
  }
#endif

#if defined(MAT_SUN_SHADOW)
// The sun's shadow for the forward pass: from the virtual shadow map under VSM (the deferred sun's test), else
// from the two cascades. Forward geometry marks no pages: vsmSunShadow takes the next coarser mip that has one.
float sunShadow(const vec3 pos, const vec3 normal) {
  const vec3 dpdx = dFdx(pos);
  const vec3 dpdy = dFdy(pos);
  if(textureSize(vsmPageTbl, 0).z==VSM_PAGE_MIPS) {
    const mat4 m      = scene.viewVirtualShadow;
    const vec4 sh0    = m*vec4(pos, 1);
    const vec4 sh1    = m*vec4(pos + dpdx, 1);
    const vec4 sh2    = m*vec4(pos + dpdy, 1);
    vec3       planeN = cross(dpdx, dpdy);
    planeN = dot(planeN,planeN)>1e-12 ? normalize(planeN) : normal;
    if(dot(planeN, normal)<0)
      planeN = -planeN;
    return vsmSunShadow(sh0.xyz/sh0.w, sh1.xy/sh1.w, sh2.xy/sh2.w, m, planeN, 0.25, vsmPageTbl, vsmPageAtlas);
    }
  return calcShadow(vec4(pos,1), 0, scene, textureSm0, textureSm1);
  }
#endif

#if defined(FORWARD)
float lambert(const vec3 normal) {
  return clamp(dot(scene.sunDir,normal), 0.0, 1.0);
  }

float henyeyGreenstein(float cosTheta, float g) {
  //g = clamp(g, -0.99, 0.99);
  float g2 = g * g;
  float denom = 1.0 + g2 - 2.0 * g * cosTheta;
  return (1.0 - g2) / (4.0 * M_PI * pow(denom, 1.5));
  }

vec3 diffuseLight(float a) {
  vec3  norm   = normalize(shInp.normal);
#if (MESH_TYPE==T_PFX)
  vec3  view   = normalize(shInp.pos - scene.camPos);
  float light  = henyeyGreenstein(-dot(view,scene.sunDir), a*0.63);
#else
  float light  = lambert(norm);
#endif
  float shadow = sunShadow(shInp.pos, norm);

  vec3  lcolor  = scene.sunColor * light * shadow;
  vec3  ambient = scene.ambient + (norm.y*0.25+0.75) * NightAmbient * Fd_Lambert;
  vec3  sky     = vec3(0); // TODO: irradiance

  return (lcolor + ambient + ambient);
  }

vec4 dbgLambert() {
  vec3  norm = normalize(shInp.normal);
  float l    = lambert(norm);
  return vec4(l,l,l,1.0);
  }
#endif

#if defined(MATERIAL_MAPS) && defined(MAT_UV) && (MESH_TYPE!=T_PFX) && !defined(WATER)
#define ANTITILE_MATERIAL 1

// Mikkelsen, Practical Real-Time Hex-Tiling (JCGT 2022), section 2:
// https://jcgt.org/published/0011/03/05/
// Keep the three cells and the albedo-derived weights for every material map.
struct AntitileCell {
  vec2 uv;
  vec2 cs;
  vec2 duvdx, duvdy;
  };

struct MaterialSampling {
  vec2 uv;
  vec2 duvdx, duvdy;
#if defined(GBUFFER)
  vec3 dpdx, dpdy;
#endif
  AntitileCell a, b, c;
  vec3 weights;
  bool antitile;
  };

vec3 antitileRandom(ivec2 cell) {
  uint seed = uint(cell.x) * 0x9E3779B9u ^ uint(cell.y) * 0x85EBCA6Bu;
  uint x = wangHash(seed);
  uint y = wangHash(seed);
  uint z = wangHash(seed);
  return vec3(x >> 8, y >> 8, z >> 8) * (1.0 / 16777216.0);
  }

AntitileCell antitileCell(ivec2 id, vec2 uv, vec2 duvdx, vec2 duvdy, float cellSize, bool rotate) {
  vec3 random = antitileRandom(id);
  float angle = rotate ? random.z * (2.0 * M_PI) : 0.0;
  float c = cos(angle), s = sin(angle);
  mat2 rotation = mat2(c,s,-s,c);
  vec2 centre = vec2(float(id.x) + 0.5 * float(id.y),
                     0.86602540378 * float(id.y)) * cellSize;
  AntitileCell cell;
  cell.uv = rotation * (uv - centre) + centre + random.xy;
  cell.cs = vec2(c,s);
  // All maps reuse these gradients; named cells avoid dynamic local arrays.
  cell.duvdx = rotation * duvdx;
  cell.duvdy = rotation * duvdy;
  return cell;
  }

void antitileGrid(inout MaterialSampling maps) {
  const float cellSize = bucket[bucketId].antitileCell;
  // Unit-distance equilateral lattice; its vertices are the hexagon centres.
  // Cell size changes only the lattice, never the source texture's UV scale.
  vec2 grid = maps.uv / cellSize;
  vec2 skew = vec2(grid.x - grid.y * 0.57735026919, grid.y * 1.15470053838);
  ivec2 base = ivec2(floor(skew));
  vec2 f = fract(skew);
  ivec2 a, b, c;
  if(f.x + f.y < 1.0) {
    a = base;
    b = base + ivec2(1,0);
    c = base + ivec2(0,1);
    maps.weights = vec3(1.0 - f.x - f.y, f.x, f.y);
    } else {
    a = base + ivec2(1,1);
    b = base + ivec2(0,1);
    c = base + ivec2(1,0);
    maps.weights = vec3(f.x + f.y - 1.0, 1.0 - f.x, 1.0 - f.y);
    }

  const bool rotate = (bucket[bucketId].flags & BK_ANTITILE_ROTATE)!=0;
  maps.a = antitileCell(a, maps.uv, maps.duvdx, maps.duvdy, cellSize, rotate);
  maps.b = antitileCell(b, maps.uv, maps.duvdx, maps.duvdy, cellSize, rotate);
  maps.c = antitileCell(c, maps.uv, maps.duvdx, maps.duvdy, cellSize, rotate);
  }

vec4 antitileDiffuse(inout MaterialSampling maps, uint tId) {
  antitileGrid(maps);
#if defined(BINDLESS)
  nonuniformEXT uint texId = tId;
#else
  const uint texId = 0;
#endif
  vec4 a = textureGrad(sampler2D(textureMain[texId], samplerMain), maps.a.uv, maps.a.duvdx*scene.mipGradScale, maps.a.duvdy*scene.mipGradScale);
  vec4 b = textureGrad(sampler2D(textureMain[texId], samplerMain), maps.b.uv, maps.b.duvdx*scene.mipGradScale, maps.b.duvdy*scene.mipGradScale);
  vec4 c = textureGrad(sampler2D(textureMain[texId], samplerMain), maps.c.uv, maps.c.duvdx*scene.mipGradScale, maps.c.duvdy*scene.mipGradScale);
  const vec3 luminance = vec3(0.299, 0.587, 0.114);
  vec3 brightness = vec3(dot(a.rgb,luminance), dot(b.rgb,luminance), dot(c.rgb,luminance));
  // The exponent limits ghosting, while luminance breaks up straight seams.
  vec3 w = max(maps.weights,vec3(0));
  vec3 w2 = w * w;
  vec3 w4 = w2 * w2;
  maps.weights = mix(vec3(1.0), brightness, 0.6) * (w4 * w2 * w);
  maps.weights /= dot(maps.weights,vec3(1));
  return a * maps.weights.x + b * maps.weights.y + c * maps.weights.z;
  }
#endif

#if defined(MAT_UV) && (MESH_TYPE!=T_PFX)
// A strand's ribbon (strandRibbon, vertex_process.glsl): its colour is looked up in the hair's ramp by the strand's
// tint, its alpha is the hair's share of the ribbon's width
vec4 strandTexel() {
  const float tint  = floor(shInp.uv.y);
  const float cover = clamp((shInp.uv.y - tint)/0.98, 0.0, 1.0);
#if defined(DEPTH_ONLY)
  return vec4(0,0,0,cover);
#else
#if defined(BINDLESS)
  nonuniformEXT uint tId = bucketId;
#else
  const         uint tId = 0;
#endif
  return vec4(textureLod(sampler2D(textureMain[tId], samplerMain), vec2((tint+0.5)/256.0, 0.5), 0.0).rgb, cover);
#endif
  }
#endif

#if defined(MAT_UV)
#if defined(ANTITILE_MATERIAL)
vec4 diffuseTex(out MaterialSampling maps) {
#else
vec4 diffuseTex() {
#endif
#if defined(ANTITILE_MATERIAL)
  // Take derivatives before any nonuniform animation/material/cell branch.
  vec2 scaledUv = shInp.uv * bucket[bucketId].uvScale;
  maps.duvdx = dFdx(scaledUv);
  maps.duvdy = dFdy(scaledUv);
#if defined(GBUFFER)
  maps.dpdx = dFdx(shInp.pos);
  maps.dpdy = dFdy(shInp.pos);
#endif
#endif
#if !defined(SIMPLE_MAT) && (MESH_TYPE!=T_PFX)
  ivec2 texAniMapDirPeriod = bucket[bucketId].texAniMapDirPeriod;
  float alphaWeight        = bucket[bucketId].alphaWeight;
#else
  ivec2 texAniMapDirPeriod = ivec2(0);
  float alphaWeight        = 1;
#endif

#if !defined(SIMPLE_MAT)
  vec2 texAnim = vec2(0);
  {
    // FIXME: this not suppose to run for every-single material
    if(texAniMapDirPeriod.x!=0) {
      uint fract = scene.tickCount32 % abs(texAniMapDirPeriod.x);
      texAnim.x  = float(fract)/float(texAniMapDirPeriod.x);
      }
    if(texAniMapDirPeriod.y!=0) {
      uint fract = scene.tickCount32 % abs(texAniMapDirPeriod.y);
      texAnim.y  = float(fract)/float(texAniMapDirPeriod.y);
      }
  }
  vec2 uv = shInp.uv + texAnim;
#else
  vec2 uv = shInp.uv;
#endif

#if defined(MATERIAL_MAPS) && (MESH_TYPE!=T_PFX)
  uv *= bucket[bucketId].uvScale;
#endif

#if defined(ANTITILE_MATERIAL)
  maps.uv = uv;
  maps.antitile = (bucket[bucketId].flags & (BK_MATERIAL_MAPS | BK_ANTITILE)) ==
                  (BK_MATERIAL_MAPS | BK_ANTITILE);
#endif

#if defined(BINDLESS)
  nonuniformEXT uint tId = bucketId;
  if(bucket[bucketId].frameCount!=0)
    tId = bucket[bucketId].diffuseFrames +
          (scene.tickCount32 / bucket[bucketId].frameDuration) % bucket[bucketId].frameCount;
#else
  const         uint tId = 0;
#endif

#if defined(ANTITILE_MATERIAL)
  vec4 tex;
  if(maps.antitile)
    tex = antitileDiffuse(maps, tId);
  else
    // The material flag is uniform within the primitive (flat bucketId).
    tex = texture(sampler2D(textureMain[tId], samplerMain),uv,scene.mipBias);
#else
  vec4 tex = texture(sampler2D(textureMain[tId], samplerMain),uv,scene.mipBias);
#endif
#if (MESH_TYPE==T_PFX)
  if(shInp.flip.z>=0.0)
    tex = mix(tex, texture(sampler2D(textureMain[tId], samplerMain), shInp.flip.xy, scene.mipBias), shInp.flip.z);
#endif

#if !defined(SIMPLE_MAT)
  tex.a *= alphaWeight;
#endif

  // return vec4(1,1,1, tex.a);
  return tex;
  }
#endif

#if defined(GBUFFER)
#if defined(MATERIAL_MAPS) && (MESH_TYPE!=T_PFX)
vec3 mappedNormal(vec3 normal, vec3 dpdx, vec3 dpdy, vec2 duvdx, vec2 duvdy, vec3 tangentNormal) {
  // Cotangent frame: Gothic meshes have no stored tangents. This also follows
  // mirrored UVs and uses the same scaled coordinates as all three textures.
  vec3 n = normalize(normal);
  vec3 py = cross(dpdy, n);
  vec3 px = cross(n, dpdx);
  vec3 t = py * duvdx.x + px * duvdy.x;
  vec3 b = py * duvdx.y + px * duvdy.y;
  float len2 = max(dot(t,t), dot(b,b));
  if(len2 <= 1e-20)
    return n;
  return normalize(mat3(t * inversesqrt(len2), b * inversesqrt(len2), n) * tangentNormal);
  }

vec3 mappedNormal(vec3 normal, vec3 dpdx, vec3 dpdy, vec2 duvdx, vec2 duvdy, vec4 tex) {
  vec2 xy = tex.ag * 2.0 - 1.0; // DXT5nm: X in alpha, Y in green (linear).
  vec3 tangentNormal = vec3(xy, sqrt(max(0.0, 1.0 - dot(xy,xy))));
  return mappedNormal(normal, dpdx, dpdy, duvdx, duvdy, tangentNormal);
  }

vec2 antitileSlope(uint tId, AntitileCell cell) {
#if defined(BINDLESS)
  nonuniformEXT uint texId = tId;
#else
  const uint texId = 0;
#endif
  vec4 nm = textureGrad(sampler2D(textureNormal[texId], samplerMain), cell.uv, cell.duvdx*scene.mipGradScale, cell.duvdy*scene.mipGradScale);
  vec2 xy = nm.ag * 2.0 - 1.0;
  float z = sqrt(max(0.0, 1.0 - dot(xy,xy)));
  // q = R * uv: the height gradient (and normal XY) returns with R^T.
  vec2 rotated = vec2(cell.cs.x * xy.x + cell.cs.y * xy.y,
                     -cell.cs.y * xy.x + cell.cs.x * xy.y);
  return rotated / max(z,1e-3);
  }

vec3 antitileOrm(uint tId, AntitileCell cell) {
#if defined(BINDLESS)
  nonuniformEXT uint texId = tId;
#else
  const uint texId = 0;
#endif
  return textureGrad(sampler2D(textureOrm[texId], samplerMain), cell.uv, cell.duvdx*scene.mipGradScale, cell.duvdy*scene.mipGradScale).rgb;
  }
#endif

#if defined(MATERIAL_MAPS) && (MESH_TYPE!=T_PFX)
// Embers do not flicker like flames: heat creeps over them in patches of about 20-40 cm over seconds, 0.55..1.
// The clock wraps at 20 pi seconds, where every term here completes whole periods.
float emberBreath(vec3 pos) {
  const float t = float(scene.tickCount32 % 62832u)*0.001;
  const float a = sin(pos.x*0.23 + t*1.1) * sin(pos.z*0.19 - t*0.7);
  const float b = sin((pos.x - pos.z)*0.11 + pos.y*0.3 + t*1.9);
  return 0.775 + 0.225*(0.6*a + 0.4*b);
  }
#endif

#if defined(ANTITILE_MATERIAL)
void mainGBuffer(vec4 t, MaterialSampling maps) {
#else
void mainGBuffer(vec4 t) {
#endif
  outDiffuse.rgb = t.rgb;
  outDiffuse.a   = encodeHintBits();
  outNormal      = encodeNormal(shInp.normal);
#if defined(MATERIAL_MAPS)
  outOrm = vec4(1,1,0,0);
#if (MESH_TYPE!=T_PFX)
  // The record bit distinguishes own materials even if their ORM equals the
  // defaults, or if the record only replaces albedo. Legacy pixels stay zero.
  outOrm.a = (bucket[bucketId].flags & BK_MATERIAL_MAPS)!=0 ? 1.0 : 0.0;
  const vec2 uv = shInp.uv * bucket[bucketId].uvScale;
  vec3 dpdx = maps.dpdx, dpdy = maps.dpdy;
  vec2 duvdx = maps.duvdx, duvdy = maps.duvdy;
#if defined(BINDLESS)
  nonuniformEXT uint tId = bucketId;
#else
  const uint tId = 0;
#endif
  if(maps.antitile) {
    const bool hasNormal = (bucket[bucketId].flags & BK_NORMAL)!=0;
    const bool hasOrm = (bucket[bucketId].flags & BK_ORM)!=0;
    const float peak = max(maps.weights.x, max(maps.weights.y, maps.weights.z));
    const uint dominant = maps.weights.x==peak ? 0u : (maps.weights.y==peak ? 1u : 2u);
    vec2 slope = vec2(0);
    vec3 orm = vec3(0);
    if(peak>=0.7) {
      AntitileCell cell;
      if(dominant==0u)
        cell = maps.a;
      else if(dominant==1u)
        cell = maps.b;
      else
        cell = maps.c;
      if(hasNormal)
        slope = antitileSlope(tId, cell);
      if(hasOrm)
        orm = antitileOrm(tId, cell);
      } else {
      // Reach the single-cell result continuously before dropping its two
      // zero-weight fetches. Winner changes happen below the fade interval.
      vec3 winner = dominant==0u ? vec3(1,0,0) : (dominant==1u ? vec3(0,1,0) : vec3(0,0,1));
      vec3 weights = mix(maps.weights, winner, smoothstep(0.6,0.7,peak));
      if(hasNormal)
        slope = weights.x * antitileSlope(tId, maps.a) +
                weights.y * antitileSlope(tId, maps.b) +
                weights.z * antitileSlope(tId, maps.c);
      if(hasOrm)
        orm = weights.x * antitileOrm(tId, maps.a) +
              weights.y * antitileOrm(tId, maps.b) +
              weights.z * antitileOrm(tId, maps.c);
      }
    if(hasNormal)
      outNormal = encodeNormal(mappedNormal(shInp.normal, dpdx, dpdy, duvdx, duvdy,
                                           vec3(slope,1)));
    if(hasOrm)
      outOrm.rgb = orm;
    } else {
    if((bucket[bucketId].flags & BK_NORMAL)!=0) {
      vec4 nm = textureGrad(sampler2D(textureNormal[tId], samplerMain), uv, duvdx*scene.mipGradScale, duvdy*scene.mipGradScale);
      outNormal = encodeNormal(mappedNormal(shInp.normal, dpdx, dpdy, duvdx, duvdy, nm));
      }
    if((bucket[bucketId].flags & BK_ORM)!=0)
      outOrm.rgb = textureGrad(sampler2D(textureOrm[tId], samplerMain), uv, duvdx*scene.mipGradScale, duvdy*scene.mipGradScale).rgb;
    }
  if((bucket[bucketId].flags & BK_HAIR)!=0) {
    // Hair cards. The strands run along v, the root at v=0, so the strand's direction on the surface is where v
    // grows: the cotangent frame's b, as in mappedNormal(). It goes into the G-buffer as an angle around the
    // normal, in the channel of metal, which hair has none of; the normal there is the one the light passes will
    // decode. r and g stay the hair map's shade and strand number
    const vec3 n  = normalize(shInp.normal);
    const vec3 py = cross(dpdy, n);
    const vec3 px = cross(n, dpdx);
    vec3 dir = py*duvdx.y + px*duvdy.y;
    // the cotangent frame turns over where the vertex normal looks away from the face the eye sees: a card that
    // stands off the hair's surface near its horizon
    if(dot(n, cross(dpdx, dpdy))<0.0)
      dir = -dir;
    if((bucket[bucketId].flags & BK_STRAND)!=0) {
      // A strand's ribbon. Its direction is the one along which uv.x, -1..1 across the ribbon, stays the same;
      // the ribbon turns the same way to every eye, so the sign holds. The normal carries the baked light in
      // its length (strandRibbon); the strand's number for the highlight comes from its tint
      dir          = dpdy*duvdx.x - dpdx*duvdy.x;
      outNormal    = encodeNormal(n);
      outOrm.r     = clamp(length(shInp.normal)*2.0 - 1.0, 0.0, 1.0);
      outOrm.g     = fract(floor(shInp.uv.y)*0.61803399);
      }
    const vec3 nq = decodeNormal(outNormal);
    dir -= nq*dot(nq, dir);
    if(dot(dir,dir)>1e-20) {
      outOrm.b     = encodeHairAngle(nq, dir);
      outOrm.a     = 1.0;
      outDiffuse.a = float(int(outDiffuse.a*255.0 + 0.5) | GBufHairBit)/255.0;
      } else {
      outOrm.b = 0.0; // a degenerate UV: plain Lambert, not a strand of unknown direction
      }
    }
  const float glow = bucket[bucketId].emissive;
  if(glow>0.0 && !maps.antitile && (bucket[bucketId].flags & BK_ORM)!=0) {
    // the ORM alpha marks the glowing texels (cracks of a burning log, the ember bed); they breathe slowly. Glowing
    // charcoal is black under its own light: a lamp must not paint the heat as orange wood (r5), so the albedo goes
    // dark by the share and the ambient pass adds the glow in the colour of its heat (emberColour, common.glsl)
    const float mask = textureGrad(sampler2D(textureOrm[tId], samplerMain), uv, duvdx*scene.mipGradScale, duvdy*scene.mipGradScale).a;
    const float share = clamp(glow*mask*emberBreath(shInp.pos), 0.0, 1.0);
    outOrm.a = 1.0 - EmissiveOrmCode*share;
    outDiffuse.rgb *= 1.0 - share;
    }
#endif
#endif
  // outNormal      = vec4(flatNormal()*0.5 + vec3(0.5), 1.0);
#if DEBUG_DRAW
  outDiffuse.rgb *= debugColors[debugId%debugColors.length()];
#endif
  }
#endif

#if defined(MAT_FOG)
// Translucent surfaces don't write depth, so the fog pass, drawn before them, fogs what lies behind.
// Here they take the fog of their own depth: in-scattered light (rgb) and transmittance (a).
vec4 fogOwnDepth() {
  if(scene.fogQuality==0 || scene.underWater!=0)
    return vec4(0,0,0,1);

  const vec2  uv = gl_FragCoord.xy*scene.screenResInv;
  const float dZ = linearDepth(gl_FragCoord.z, scene.clipInfo);
  const float d0 = linearDepth(dFogMin,        scene.clipInfo);
  const float d1 = linearDepth(dFogMax,        scene.clipInfo);
  const float d  = (dZ-d0)/(d1-d0);

  // as fog.frag: the scattering and the transmittance fade in from the camera over half the LUT's first slice
  const float near = min(d*float(textureSize(fogLut,0).z)*2.0, 1.0);
  vec4 val = textureLod(fogLut, vec3(uv,d), 0);
  if(scene.fogQuality==2) {
    // as fog.frag (VOLUMETRIC), without the shadow occlusion of single scattering.
    // Under GI the multiple scattering as fog.frag (FOG_SKY_VIS) lets it: by mix(the sun's bit, 1, the sky over the
    // camera) along the ray. The bits are those of the fog pass for this pixel (32 steps to the opaque surface behind);
    // their lit share stands in for the lit share of the scattering up to this surface (step-2-shade.md, «Дымка
    // прозрачного»). Exactly 1 without GI: the bits are not read
    const vec4 ms = textureLod(fogLutMs, vec3(uv,d), 0);
    float k = 1.0;
    if(scene.fogSkyVis<1.0) {
      const ivec2 occSize  = imageSize(fogOcclusion);
      const ivec2 occRatio = (scene.screenRes + occSize - 1)/occSize;
      const uint  bits     = imageLoad(fogOcclusion, min(ivec2(gl_FragCoord.xy)/occRatio, occSize-1)).r;
      const float lit      = float(bitCount(bits))/32.0;
      k = scene.fogSkyVis + (1.0-scene.fogSkyVis)*lit;
      }
    val = vec4(val.rgb + ms.rgb*k, ms.a);
    }
  return vec4(val.rgb*near*scene.GSunIntensity*scene.exposure, mix(1.0, val.a, near));
  }
#endif

#if defined(SOFT_PARTICLE)
// A sprite fades over the last cm before the opaque scene behind it (a flame against a pot, a log, the ground)
// and next to the camera, so no straight seam shows where its quad cuts through geometry.
float softParticle() {
  const float behind = linearDepth(texelFetch(gbufferDepth, ivec2(gl_FragCoord.xy), 0).r, scene.clipInfo);
  const float own    = linearDepth(gl_FragCoord.z, scene.clipInfo);
  const float touch  = clamp((behind-own)/SoftParticleDepth, 0.0, 1.0);
  const float near   = clamp((own-SoftParticleNear.x)/(SoftParticleNear.y-SoftParticleNear.x), 0.0, 1.0);
  return touch*touch*(3.0-2.0*touch) * near;
  }
#endif

#if defined(TAA_REACTIVE) && !defined(GHOST)
// the material's weight: particles by their effect, meshes by a texture animation (Material::taaReactive)
float reactiveWeight() {
#if (MESH_TYPE==T_PFX)
  return pushPfx.reactive;
#else
  return bucket[bucketId].reactive;
#endif
  }
#endif

#if defined(FORWARD)
void mainForward(vec4 t) {
  vec3  color = t.rgb;
  float alpha = t.a;

#if defined(ATEST)
  alpha = (alpha-0.5)*2.0;
#endif

  const vec3 albedo = textureAlbedo(color.rgb);
  color = albedo * diffuseLight(alpha) * scene.exposure;
#if (MESH_TYPE==T_PFX)
  // smoke and steam lit by the fires next to them, as light.frag lights a surface
  color += albedo * shInp.pointLight * max(1.0, scene.exposure) * Fd_Lambert * 0.25;
#endif

  const vec4 fog = fogOwnDepth();
  color = color*fog.a + fog.rgb;

#if defined(SOFT_PARTICLE)
  alpha *= softParticle();
#endif
  outColor = vec4(color,alpha);
#if defined(TAA_REACTIVE)
  // SrcAlpha, 1-SrcAlpha: mask = w*a + mask*(1-a), the opacity of the reactive; a still surface in front hides it
  outReactive = vec4(reactiveWeight(), 0, 0, alpha);
#endif
  }
#endif

#if defined(EMISSIVE)
void mainEmissive(vec4 t) {
  vec3 color = textureEmmisive(t.rgb);
#if (MESH_TYPE==T_PFX) && !defined(MULTIPLY)
  // A flame keeps its own luminance, so it reads brighter than the scene as the exposure rises at dusk and at
  // night; the factor leaves it as it was under the noon sun (EmissiveSunRatio, common.glsl). The exposure does
  // not see the fire itself, and the night's is hundreds of times the noon's: the gain stops at EmissiveMaxGain,
  // as an eye that adapts to the fire it looks at.
  color *= min(EmissiveSunRatio * scene.GSunIntensity * scene.exposure, EmissiveMaxGain);
#endif
#if defined(MAT_FOG)
  color *= fogOwnDepth().a;
#endif
  float alpha = t.a;
#if defined(SOFT_PARTICLE)
  alpha *= softParticle();  // additive with SrcAlpha: the alpha weighs the added light
#endif
  outColor = vec4(color,alpha);
#if defined(TAA_REACTIVE)
  // SrcAlpha, One: mask += w*e*a, the added light in shares of Lref (on screen, after the exposure)
  const float e = clamp(dot(color, vec3(0.25, 0.5, 0.25))*scene.reactiveLrefInv, 0.0, 1.0);
  outReactive = vec4(reactiveWeight()*e, 0, 0, alpha);
#endif
  }
#endif

#if defined(GHOST)
void mainGhost(vec4 t) {
  vec3  color  = textureAlbedo(t.rgb) * 5.0;
  vec3  normal = normalize(shInp.normal);

  normal = (scene.viewProject*vec4(normal,0.0)).xyz;

  vec2  fragCoord = (gl_FragCoord.xy*scene.screenResInv)*2.0-vec2(1.0);
  fragCoord += normal.xy * 0.01;

  vec4 back = textureLod(sceneColor, (fragCoord*0.5+0.5), 0);

  // sceneColor is stashed before the fog pass
  const vec4 fog = fogOwnDepth();
  outColor = vec4(mix(back.rgb * color, back.rgb, vec3(0.6))*fog.a + fog.rgb, t.a);
#if defined(TAA_REACTIVE)
  outReactive = vec4(0); // no blending: the refracted scene behind is not reactive
#endif
  }
#endif

#if defined(WATER)
vec4 underWaterColorDepth(vec3 normal) {
  const vec2  fragCoord = (gl_FragCoord.xy*scene.screenResInv)*2.0-vec2(1.0);
  const float ior       = IorWater;
  //return vec4(0);

  const vec3  camPos = scene.camPos;
  const vec3  view   = normalize(shInp.pos - camPos);
  const vec3  refr   = refract(view, normal, ior);

  vec3        back   = texelFetch(sceneColor,   ivec2(gl_FragCoord.xy), 0).rgb;
  const float depth  = texelFetch(gbufferDepth, ivec2(gl_FragCoord.xy), 0).r;

  const float ground = unproject(depth);
  const float water  = unproject(gl_FragCoord.z);
  float       dist   = (ground-water);

  const vec2 p2 = (gl_FragCoord.xy*scene.screenResInv) + normal.xz * min(dist*0.01,1.0) * 0.1;

  float depth2 = textureLod(gbufferDepth, p2, 0).r;
  if(depth2>gl_FragCoord.z) {
    back   = textureLod(sceneColor, p2, 0).rgb;
    const float ground2 = unproject(depth2);
    dist = (ground2-water);
    } else {
    depth2 = depth;
    }

  vec4 fragPos1 = scene.viewProjectInv*vec4(fragCoord,depth2,1.0);
  fragPos1.xyz /= fragPos1.w;

  //return vec4(back,length(fragPos1.xyz - fragPos0.xyz));
  return vec4(back,length(fragPos1.xyz - shInp.pos.xyz));
  }

vec3 waterScatter(vec3 back, vec3 normal, float len) {
  vec3  transmittance = waterTransmittance(len);
  // note: less sun light and less obsevable light
  transmittance = transmittance*transmittance;

  const float f       = fresnel(scene.sunDir,normal,IorWater);
  const vec3  scatter = f * scene.sunColor * (1-exp(-len/20000.0)) * max(scene.sunDir.y, 0);
  return (back + scatter*scene.exposure)*transmittance;
  }

vec4 waterShading(vec4 t, const vec3 normal) {
  const bool underWater = (scene.underWater!=0);

  const float ior = underWater ? IorAir : IorWater;

  vec4 camPos = scene.viewProjectInv*vec4(0,0,0,1.0);
  camPos.xyz /= camPos.w;

  const vec3  view   = normalize(shInp.pos - camPos.xyz);
  const vec3  refr   = refract(view, normal, ior);
        vec3  refl   = reflect(view, normal);

  const float f      = fresnel(refl,normal,ior);

  if(underWater) {
    vec3 back = texelFetch(sceneColor, ivec2(gl_FragCoord.xy), 0).rgb;
    return vec4(back.rgb * (1.0-f),1);
    }

  const vec4 back  = underWaterColorDepth(normal);
  const vec3 color = waterScatter(back.rgb, normal, back.a) * (1.0-f);
  // color = waterColor(color,normal) * WaterAlbedo ;
  return vec4(color,1);
  }

void mainWater(vec4 t) {
  const float waveMaxAmplitude = bucket[bucketId].waveMaxAmplitude;
  const float turbidity        = bucket[bucketId].turbidity;
  const float turbidityDepth   = bucket[bucketId].turbidityDepth;
  // Water in a pot (it has a turbidity) ripples at the pot's scale: the sea's waves, 25 times smaller and slower.
  const float rippleScale      = turbidity>0.0 ? 25.0 : 1.0;

  vec3 lx = dFdx(shInp.pos), ly = dFdy(shInp.pos);
  float minLength = max(length(lx),length(ly));

  Wave wx = wave(shInp.pos*rippleScale, minLength*rippleScale, waveIterationsHigh, waveAmplitude(waveMaxAmplitude));

  if(gl_FrontFacing) {
    // BROKEN: water mesh is two sided
    wx.normal = -wx.normal;
    }

  outColor = waterShading(t,wx.normal);
  const float shadow = scene.underWater==0 ? sunShadow(shInp.pos, normalize(shInp.normal)) : 0.0;
  if(turbidity>0.0) {
    // Water in a pot clouded with ash and fat, or a stew: the material's colour, lit like a surface, over the clear water.
    // With a depth (cm) the cloud thickens with the liquid it is seen through (Beer-Lambert): a stew is clearer at the wall.
    float cloud = turbidity;
    if(turbidityDepth>0.0)
      cloud *= 1.0 - exp(-underWaterColorDepth(wx.normal).a/turbidityDepth);
    const vec3  n   = normalize(shInp.normal);
    const vec3  sun = scene.sunColor * clamp(dot(scene.sunDir,n), 0.0, 1.0) * shadow * Fd_Lambert; // as direct_light.frag
    const vec3  amb = scene.ambient + (n.y*0.25+0.75) * NightAmbient * Fd_Lambert; // x2 below stands for the sky irradiance
    outColor.rgb = mix(outColor.rgb, textureAlbedo(t.rgb) * (sun + 2.0*amb) * scene.exposure, cloud);
    }
  if(scene.underWater==0) {
    // Sun glint: the sun reflected by the rippled surface, not in shadow (water_reflection.frag adds only the sky).
    // A sharp GGX lobe over the sun's irradiance: in life it is the brightest thing on the water.
    const vec3 view = normalize(scene.camPos - shInp.pos);
    const vec3 n    = wx.normal.y<0.0 ? -wx.normal : wx.normal;
    const vec3 spec = materialSpecular(n, view, scene.sunDir, vec3(WaterF0), turbidity>0.0 ? PotWaterRoughness : WaterRoughness);
    outColor.rgb += spec * scene.sunColor * max(dot(n, scene.sunDir), 0.0) * shadow * scene.exposure;
    }
  outDiffuse.rgb = t.rgb;
  outDiffuse.a   = encodeHintBits();
  outNormal      = encodeNormal(wx.normal);
  }
#endif

#if defined(GBUFFER)
// Motion vectors of objects (graphics plan 1b): where the point was a frame ago minus where it would be had it stood
// still, texture coordinates, current -> previous (Taa::motionResidual). Landscape and particles: 0
vec2 motionResidual() {
#if defined(MAT_MOTION)
  if(scene.motionVectors==0u)
    return vec2(0);
  const vec3  s  = shInp.prevStatic;
  const vec3  d  = shInp.prevDelta;
  const float w0 = s.z;
  const float w1 = s.z + d.z;
  if(w0<=0.0 || w1<=0.0)
    return vec2(0); // behind the previous camera: no history to point at
  // ((s.xy + d.xy)/w1 - s.xy/w0)*0.5 over one denominator: exactly 0 for d=0, also by the near plane, where the two
  // divisions of the plain form round apart by up to pixels
  return (d.xy*w0 - s.xy*d.z)/(w0*w1)*0.5;
#else
  return vec2(0);
#endif
  }
#endif

// The alpha test's threshold. Our glTF MASK (BK_HASHED_ALPHA: brows, lashes, hair cards) under TAA takes a threshold
// of its own per pixel and frame (reports/content/2026-11-npc-hair): from the pixel's interleaved gradient noise it
// steps by the golden ratio each TAA frame, so a texel of alpha 0.3 stays in 30% of the frames, and the history
// averages the edge to the texel's coverage without sorting. Neighbours differ, so the resolve's 3x3 box takes the
// spread as legit. A hash of the texture cell under the pixel shifts the start: two cards over one pixel take
// different thresholds and their coverages add up, where one threshold would let the front card hide the back.
// The occluders drawn into the main depth ahead of the G-buffer (Renderer::drawHiZ) keep only what no threshold
// drops, else the G-buffer would find the card's depth in front of the skin it lets through. Shadows and frames
// without TAA keep 0.5
float alphaThreshold() {
#if defined(ATEST) && defined(MAT_UV) && (MESH_TYPE!=T_PFX)
  if((scene.viewFlags & VF_TAA)==0u || (bucket[bucketId].flags & BK_HASHED_ALPHA)==0u)
    return 0.5;
#if defined(GBUFFER)
  const uvec2 cell  = uvec2(ivec2(floor(shInp.uv*256.0)) & 0xFFFF);
  const float layer = float(pcgHash(cell.x | (cell.y << 16)) >> 8) * (1.0/16777216.0);
  const float step  = float(scene.taaFrame % 64u) * 0.61803399;
  return max(fract(interleavedGradientNoise(gl_FragCoord.xy) + layer + step), 0.5/255.0);
#elif defined(DEPTH_ONLY) && !defined(VIRTUAL_SHADOW)
  return (scene.viewFlags & VF_HIZ)!=0u ? 1.0 - 0.5/255.0 : 0.5;
#else
  return 0.5;
#endif
#else
  return 0.5;
#endif
  }

void main() {
#if defined(MAT_UV)
#if defined(ANTITILE_MATERIAL)
  MaterialSampling maps;
  vec4 t = diffuseTex(maps);
#else
  vec4 t = diffuseTex();
#endif
#  if (MESH_TYPE!=T_PFX)
  if((bucket[bucketId].flags & BK_STRAND)!=0u)
    t = strandTexel();
#  endif
#  if defined(ATEST) && !(defined(GBUFFER) && defined(MATERIAL_MAPS))
  if(t.a<alphaThreshold())
    discard;
#  endif
#endif

#if defined(ATEST) && defined(GBUFFER) && defined(MATERIAL_MAPS)
  const float alphaTest = t.a;
#endif
#if defined(MAT_COLOR)
  t *= shInp.color;
#endif

#if defined(GBUFFER)
#if defined(ANTITILE_MATERIAL)
  mainGBuffer(t, maps);
#else
  mainGBuffer(t);
#endif
  outMotion = motionResidual();
#if defined(ATEST) && defined(MATERIAL_MAPS)
  if(alphaTest<alphaThreshold())
    discard;
#endif
#elif defined(WATER)
  mainWater(t);
#elif defined(FORWARD) && !defined(DEPTH_ONLY)
  mainForward(t);
#elif defined(EMISSIVE) && !defined(DEPTH_ONLY)
  mainEmissive(t);
#elif defined(GHOST) && !defined(DEPTH_ONLY)
  mainGhost(t);
#endif

#if DEBUG_DRAW && !defined(GBUFFER) && !defined(DEPTH_ONLY)
  outColor   = vec4(debugColors[debugId%debugColors.length()],1.0);
#endif

  //outColor = vec4(inZ.xyz/inZ.w,1.0);
  //outColor = vec4(vec3(inPos.xyz)/1000.0,1.0);
  //outColor = vec4(vec3(shMap),1.0);
  //outColor = vec4(vec3(calcLight()),1.0);
  //outColor = vec4(vec3(calcShadow()),1.0);
  //vec3 shPos0  = (shInp.shadowPos[0].xyz)/shInp.shadowPos[0].w;
  //outColor   = vec4(vec3(shPos0.xy,0),1.0);
  //outColor = dbgLambert();
  }

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
#endif
#elif defined(WATER)
layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outDiffuse;
layout(location = 2) out uint outNormal;
#elif !defined(DEPTH_ONLY)
layout(location = 0) out vec4 outColor;
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
  float shadow = calcShadow(vec4(shInp.pos,1), 0, scene, textureSm0, textureSm1);

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
  vec4 a = textureGrad(sampler2D(textureMain[texId], samplerMain), maps.a.uv, maps.a.duvdx, maps.a.duvdy);
  vec4 b = textureGrad(sampler2D(textureMain[texId], samplerMain), maps.b.uv, maps.b.duvdx, maps.b.duvdy);
  vec4 c = textureGrad(sampler2D(textureMain[texId], samplerMain), maps.c.uv, maps.c.duvdx, maps.c.duvdy);
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
    tex = texture(sampler2D(textureMain[tId], samplerMain),uv);
#else
  vec4 tex = texture(sampler2D(textureMain[tId], samplerMain),uv);
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
  vec4 nm = textureGrad(sampler2D(textureNormal[texId], samplerMain), cell.uv, cell.duvdx, cell.duvdy);
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
  return textureGrad(sampler2D(textureOrm[texId], samplerMain), cell.uv, cell.duvdx, cell.duvdy).rgb;
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
      vec4 nm = textureGrad(sampler2D(textureNormal[tId], samplerMain), uv, duvdx, duvdy);
      outNormal = encodeNormal(mappedNormal(shInp.normal, dpdx, dpdy, duvdx, duvdy, nm));
      }
    if((bucket[bucketId].flags & BK_ORM)!=0)
      outOrm.rgb = textureGrad(sampler2D(textureOrm[tId], samplerMain), uv, duvdx, duvdy).rgb;
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

  vec4 val = textureLod(fogLut, vec3(uv,d), 0);
  if(scene.fogQuality==2) {
    // as fog.frag (VOLUMETRIC), without the shadow occlusion of single scattering
    const vec4 ms = textureLod(fogLutMs, vec3(uv,d), 0);
    val = vec4(val.rgb*min(d*64.0, 1.0) + ms.rgb, ms.a);
    }
  return vec4(val.rgb*scene.GSunIntensity*scene.exposure, val.a);
  }
#endif

#if defined(FORWARD)
void mainForward(vec4 t) {
  vec3  color = t.rgb;
  float alpha = t.a;

#if defined(ATEST)
  alpha = (alpha-0.5)*2.0;
#endif

  color = textureAlbedo(color.rgb);
  color *= diffuseLight(alpha);
  color *= scene.exposure;

  const vec4 fog = fogOwnDepth();
  color = color*fog.a + fog.rgb;

  outColor = vec4(color,alpha);
  }
#endif

#if defined(EMISSIVE)
void mainEmissive(vec4 t) {
  vec3 color = textureEmmisive(t.rgb);
#if defined(MAT_FOG)
  color *= fogOwnDepth().a;
#endif
  outColor = vec4(color,t.a);
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

  vec3 lx = dFdx(shInp.pos), ly = dFdy(shInp.pos);
  float minLength = max(length(lx),length(ly));

  Wave wx = wave(shInp.pos, minLength, waveIterationsHigh, waveAmplitude(waveMaxAmplitude));

  if(gl_FrontFacing) {
    // BROKEN: water mesh is two sided
    wx.normal = -wx.normal;
    }

  outColor       = waterShading(t,wx.normal);
  outDiffuse.rgb = t.rgb;
  outDiffuse.a   = encodeHintBits();
  outNormal      = encodeNormal(wx.normal);
  }
#endif

void main() {
#if defined(MAT_UV)
#if defined(ANTITILE_MATERIAL)
  MaterialSampling maps;
  vec4 t = diffuseTex(maps);
#else
  vec4 t = diffuseTex();
#endif
#  if defined(ATEST) && !(defined(GBUFFER) && defined(MATERIAL_MAPS))
  if(t.a<0.5)
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
#if defined(ATEST) && defined(MATERIAL_MAPS)
  if(alphaTest<0.5)
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

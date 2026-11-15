#ifndef GRASS_LAYERS_GLSL
#define GRASS_LAYERS_GLSL

#include "grass/grass_cover.glsl"
#include "grass/grass_border.glsl"

// The meadow's own ground (reports/engine/2026-11-14-meadow-ground; materials.json "ground", BK_GROUND, and "verge"
// of the ground beside it, BK_VERGE): two layers laid by the world's x and z in place of the painted texture - the
// sward under the grass, the earth where it is bare. Which of the two a pixel is, the rule of the blades says
// (grass_cover.glsl), so the grass stands on sward and stops with it; where the rule is between none and whole the
// layers' heights (the blue of their ORM) move the line by more than the rule's own noise does: the sward ends in
// its own tufts, its low thatch gives way first, and stones lie in its edge. A pixel is one or the other: colour,
// normal and ORM all come from one.
// On a ground with `verge` only the strip along the border is touched: sward as far as its tongues come, then the
// meadow's earth about as far as the record's margin, where it gives way to the road's own paint from its low
// parts on, the end of it wandering; past the strip the pixel is the road's, as without the record.
// In the landscape's G-buffer pass with material maps and bindless textures: needs fringeTexture and mappedNormal
// of materials/main.frag.
const float GrassLayerHeight   = 140.0;  // of 256 of the cover's level: what the heights' difference is worth
const float GrassLayerFarFrom  = 1500.0; // cm: the sward's colour passes to its far repeat
const float GrassLayerFarTo    = 4000.0;
const float GrassLayerNormalTo = 3000.0; // cm: no normal map beyond; fading from 0.7 of it
const float GrassVergeSoft     = 30.0;   // cm: the earth of the strip gives way to the road over this
const float GrassVergeWander   = 10.0;   // cm: the strip's end wanders to either side, at a margin of 50 and more
const float GrassVergeRipple   = 5.0;    // cm: and by a quick noise
const float GrassLiesFrom      = 0.50;   // the normal's y: steeper ground keeps its painted texture
const float GrassLiesTo        = 0.65;

// tex: the sward's maps in the bindless arrays, the earth's are the next; swardK, earthK: repeats per cm; farK: the
// far repeat's tile of the sward's, 0 without. Returns 1 on a pixel of sward, 0 of earth; -1 where the pixel is
// not the layers'; -2 where the cover's paint took it
float grassLayers(uint flags, uint tex, float swardK, float earthK, float farK, vec3 pos, vec3 normal,
                  vec3 dpdx, vec3 dpdy, inout vec4 diffuse, inout uint normalOut, inout vec4 orm) {
  const vec4 ring = scene.grassRing;
  if(ring.w<=0.0)
    return -1.0;
  const vec3  gn   = normalize(normal);
  const float lies = smoothstep(GrassLiesFrom, GrassLiesTo, gn.y);
  if(lies<=0.0)
    return -1.0;

  const bool  verge = (flags & BK_VERGE)!=0u;
  const uint  partsOff = grassPartsOff(ring.w);
  if(!verge && (partsOff & GrassPartLayers)!=0u)
    return -1.0; // the bench's: the painted ground
  const ivec2 place = ivec2(floor(pos.xz*16.0));
  float dist, reach, margin;
  const bool  near  = grassBorderNear(pos.xz, verge ? 180.0 : float(GrassCoverInside), dist, reach, margin);
  if(verge && (!near || dist>=max(float(grassCoverBeyond(int(reach))), margin)))
    return -1.0;
  const int   border = near ? grassCoverBorder(place, verge ? int(dist*16.0) : -int(dist*16.0), int(reach)) : 256;
  const int   level  = grassCoverLevel(place, border);

  const float mip = scene.mipGradScale;
  const vec2  uvS = pos.xz*swardK, dxS = dpdx.xz*swardK, dyS = dpdy.xz*swardK;
  const vec2  uvE = pos.xz*earthK, dxE = dpdx.xz*earthK, dyE = dpdy.xz*earthK;
  bool sward = level>=256;
  vec3 ormS = vec3(0), ormE = vec3(0);
  const bool between = level>0 && level<256;
  if(between) {
    ormS  = fringeTexture(2u, tex,    uvS, dxS*mip, dyS*mip).rgb;
    ormE  = fringeTexture(2u, tex+1u, uvE, dxE*mip, dyE*mip).rgb;
    sward = float(level) + (ormS.b-ormE.b)*GrassLayerHeight>float(GrassCoverSward);
    }
  // the earth on a verge: the strip to the margin, the road's own beyond
  float share = lies;
  bool  haveE = between;
  if(!sward && verge) {
    if(!haveE)
      ormE = fringeTexture(2u, tex+1u, uvE, dxE*mip, dyE*mip).rgb;
    haveE = true;
    const float end = margin + float(grassValueNoise(place,1600,0x66u)-32640)*(GrassVergeWander*min(margin/50.0,1.0)/32640.0)
                             + float(grassValueNoise(place,320,0x65u)-32640)*(GrassVergeRipple/32640.0);
    // clods and stones are the last of the earth: its height shifts where a pixel of it ends
    share *= smoothstep(0.2, 0.8, (end-dist)/GrassVergeSoft + ormE.b);
    if(share<=0.0)
      return -1.0;
    }

  if((partsOff & GrassPartCover)!=0u) {
    diffuse.rgb = sward ? vec3(0.0,0.7,0.0) : mix(vec3(1.0,0.0,1.0), vec3(0.8,0.2,0.0), share);
    return -2.0;
    }

  const float d = distance(pos, scene.camPos);
  vec3 albedo, o;
  vec4 nm;
  const bool mapped = d<GrassLayerNormalTo;
  if(sward) {
    const float far = farK>0.0 ? smoothstep(GrassLayerFarFrom, GrassLayerFarTo, d) : 0.0;
    albedo = vec3(0);
    if(far<1.0)
      albedo = fringeTexture(0u, tex, uvS, dxS*mip, dyS*mip).rgb;
    if(far>0.0)
      albedo = mix(albedo, fringeTexture(0u, tex, uvS/farK, dxS*(mip/farK), dyS*(mip/farK)).rgb, far);
    o = between ? ormS : fringeTexture(2u, tex, uvS, dxS*mip, dyS*mip).rgb;
    if(mapped)
      nm = fringeTexture(1u, tex, uvS, dxS*mip, dyS*mip);
    } else {
    albedo = fringeTexture(0u, tex+1u, uvE, dxE*mip, dyE*mip).rgb;
    o      = haveE ? ormE : fringeTexture(2u, tex+1u, uvE, dxE*mip, dyE*mip).rgb;
    if(mapped)
      nm = fringeTexture(1u, tex+1u, uvE, dxE*mip, dyE*mip);
    }
  vec3 n = gn;
  if(mapped) {
    n = sward ? mappedNormal(normal, dpdx, dpdy, dxS, dyS, nm) : mappedNormal(normal, dpdx, dpdy, dxE, dyE, nm);
    n = normalize(mix(n, gn, smoothstep(0.7*GrassLayerNormalTo, GrassLayerNormalTo, d)));
    }
  // the blue was the height: neither layer has metal
  const vec3 own = vec3(o.r, o.g, 0.0);
  if(share<1.0) {
    // toward what the pixel was: on a steep ground its painted texture, at the end of a verge's strip the road
    diffuse.rgb = mix(diffuse.rgb, albedo, share);
    normalOut   = encodeNormal(normalize(mix(decodeNormal(normalOut), n, share)));
    orm.rgb     = mix(orm.rgb, own, share);
    } else {
    diffuse.rgb = albedo;
    normalOut   = encodeNormal(n);
    orm.rgb     = own;
    }
  orm.a = 1.0;
  return sward ? 1.0 : 0.0;
  }

#endif

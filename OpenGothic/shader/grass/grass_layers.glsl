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
// normal and ORM all come from one. The earth of a spot in the meadow is dressed as a dry place: paler, its clods
// and its low parts apart by the layer's height (grass_common.glsl, GrassSpotEarth*).
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

// The loose earth of a road with its own material (grass/grass_track.glsl): of the road's strip, and of the
// meadow's notches by it (`ground.border`). One lattice on both sides of the mesh's edge, turned from the world's
// axes so that a stone does not come back along a straight road; one slow tone over both
vec2 grassLooseUv(vec2 xz) { return vec2(0.93358*xz.x + 0.35837*xz.y, -0.35837*xz.x + 0.93358*xz.y); }
float grassTrackNoise(ivec2 p, int step, uint salt) { return float(grassValueNoise(p,step,salt)-32640)*(1.0/32640.0); }
float grassTrackTone(ivec2 place) {
  return 1.0 + grassTrackNoise(place,12800,0x79u)*0.08 + grassTrackNoise(place,2080,0x7Au)*0.05;
  }

// tex: the sward's maps in the bindless arrays, the earth's are the next; swardK, earthK: repeats per cm; farK: the
// far repeat's tile of the sward's, 0 without. otherTex, otherK: the maps and the repeats per cm of a road's loose
// earth, 0 without - on a meadow the earth of its notches by the road, on a verge the road's own material under
// the sward: a pixel of the strip that is not sward is then left to grass_track.glsl, its share in `strip`.
// Returns 1 on a pixel of sward, 0 of earth; -1 where the pixel is not the layers'; -2 where the cover's paint
// took it
float grassLayers(uint flags, uint tex, float swardK, float earthK, float farK, uint otherTex, float otherK,
                  vec3 pos, vec3 normal, vec3 dpdx, vec3 dpdy, inout vec4 diffuse, inout uint normalOut,
                  inout vec4 orm, out float strip) {
  strip = 0.0;
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
  int bare;
  const int   level  = grassCoverLevel(place, border, bare);

  const float mip = scene.mipGradScale;
  const vec2  uvS = pos.xz*swardK, dxS = dpdx.xz*swardK, dyS = dpdy.xz*swardK;
  const vec2  uvE = pos.xz*earthK, dxE = dpdx.xz*earthK, dyE = dpdy.xz*earthK;
  // the road's loose earth: under the sward of a road's strip, and in a notch of the meadow by the road - there a
  // bare spot keeps the meadow's earth, and where the two meet the heights say which a pixel is
  const bool  other = otherK>0.0;
  const vec2  uvO = grassLooseUv(pos.xz)*otherK, dxO = grassLooseUv(dpdx.xz)*otherK, dyO = grassLooseUv(dpdy.xz)*otherK;
  const bool  notch = other && !verge && near && border<256;
  bool loose = other && (verge || (notch && bare<=0));
  bool sward = level>=256;
  vec3 ormS = vec3(0), ormE = vec3(0);
  const bool between = level>0 && level<256;
  if(!sward) {
    if(between)
      ormS = fringeTexture(2u, tex, uvS, dxS*mip, dyS*mip).rgb;
    if(loose)
      ormE = fringeTexture(2u, otherTex, uvO, dxO*mip, dyO*mip).rgb; else
      ormE = fringeTexture(2u, tex+1u, uvE, dxE*mip, dyE*mip).rgb;
    if(notch && !loose) {
      const vec3 ormO = fringeTexture(2u, otherTex, uvO, dxO*mip, dyO*mip).rgb;
      if(float(256-border)*(1.0 + 2.0*(ormO.b-ormE.b))>float(bare)) {
        loose = true;
        ormE  = ormO;
        }
      }
    if(between)
      sward = float(level) + (ormS.b-ormE.b)*GrassLayerHeight>float(GrassCoverSward);
    }
  // the earth on a verge: the strip to the margin, the road's own beyond
  float share = lies;
  if(!sward && verge) {
    const float end = margin + float(grassValueNoise(place,1600,0x66u)-32640)*(GrassVergeWander*min(margin/50.0,1.0)/32640.0)
                             + float(grassValueNoise(place,320,0x65u)-32640)*(GrassVergeRipple/32640.0);
    // clods and stones are the last of the earth: its height shifts where a pixel of it ends
    const float left = smoothstep(0.2, 0.8, (end-dist)/GrassVergeSoft + ormE.b);
    share *= left;
    if(share<=0.0)
      return -1.0;
    if(other) {
      strip = left;
      return -1.0;
      }
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
    o = ormE;
    if(loose) {
      albedo = fringeTexture(0u, otherTex, uvO, dxO*mip, dyO*mip).rgb*grassTrackTone(place);
      if(mapped)
        nm = fringeTexture(1u, otherTex, uvO, dxO*mip, dyO*mip);
      } else {
      albedo = fringeTexture(0u, tex+1u, uvE, dxE*mip, dyE*mip).rgb;
      if(mapped)
        nm = fringeTexture(1u, tex+1u, uvE, dxE*mip, dyE*mip);
      }
    }
  // the earth of a spot in the meadow, not of a border: there it stays the road's
  const float spot = (sward || verge || loose) ? 0.0 : smoothstep(0.0, 0.3, float(bare)/256.0);
  if(spot>0.0) {
    const float high = smoothstep(0.25, 0.75, o.b);
    const float slow = float(grassValueNoise(place,560,0x68u)-32640)*(GrassSpotEarthSlow/32640.0);
    albedo *= mix(vec3(1.0), mix(GrassSpotEarthLow, GrassSpotEarthHigh, high)*(1.0+slow), spot);
    o.r    *= mix(1.0, mix(GrassSpotEarthAo, 1.0, high), spot);
    }
  vec3 n = gn;
  if(mapped) {
    if(sward) {
      n = mappedNormal(normal, dpdx, dpdy, dxS, dyS, nm);
      } else {
      // DXT5nm, as mappedNormal reads it; the slope steeper on a spot
      const vec2 xy = nm.ag*2.0 - 1.0;
      n = mappedNormal(normal, dpdx, dpdy, loose ? dxO : dxE, loose ? dyO : dyE,
                       vec3(xy*mix(1.0, GrassSpotEarthBump, spot), sqrt(max(0.0, 1.0 - dot(xy,xy)))));
      }
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

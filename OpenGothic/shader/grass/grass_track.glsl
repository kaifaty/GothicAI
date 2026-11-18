#ifndef GRASS_TRACK_GLSL
#define GRASS_TRACK_GLSL

#include "grass/grass_layers.glsl"

// The cart track's own material (reports/engine/2026-11-15-meadow-road; materials.json "track", BK_TRACK) in place
// of the road's painted texture. What has a direction comes from three small maps in the mesh's own uv, the
// ribbon's: a tint (the ruts' beds darker), a normal (the ruts' walls, the crown) and a mask - red the occlusion of
// the large forms, green the wear (where the earth is loose: the ruts and the shoulders), blue the ribbon's own
// share (0 at its edges, where the paint stays). What has none lies by the world's x and z in two layers: the dry
// crust and the loose earth. A pixel is one or the other: the wear says which, moved by the world's noise and by
// the layers' heights (the blue of their ORM), so the ruts' edges widen, narrow and stain, and stones lie in them.
// The ribbon's drawing itself wanders by the world's noise, the ruts lose their depth and tone in places, and the
// crust left in a rut keeps only GrassTrackCrustTint of its dark: a profile alone would draw ruled bands along
// the road.
// The road's colour is the painted one's palette: the crust has the red-brown of the painted earth, and stains
// lie on the body by the world's noise - darker and redder (GrassTrackStain), none at the ribbon's edge and none
// in a rut that has not faded: the tint's dark there times a stain is a streak of wine along the road. Where the
// edge is the tint says: its lightest channel falls from 1 there to GrassTrackBody inside; where a rut is, its
// green does (the shoulders keep theirs).
// The loose layer is one earth with the meadow beside: where the sward of a verge ends on the road (grass_layers.
// glsl, `strip`) the pixel is the loose earth whatever the ribbon says, by the same lattice and tone as the earth
// of the meadow's notches. Outside a meadow the ribbon's edge stays painted.
// From GrassTrackFarFrom on the two layers are mixed by the wear and the ORM is a constant: the tone and the
// ribbon's drawing stay, the grain is gone with the distance - and the drawing itself is read softer, by a foot of
// GrassTrackFarSoft: wide soft ruts, as a road's are from 15 m. What the grain's normals and occlusion took from
// the light near (0.88 by the layers' slopes alone) stays as GrassTrackFarShade: the painted road too is darker
// from afar - its mips are means of the encoded colour, 0.86-0.89 of the linear mean at the third and fourth. By
// the frames the road with its stains stands at 0.94-0.96 of the painted one near and, with 0.86 of this shade,
// at 1.04-1.05 from afar.
// Work outside the track's share and samples that cannot change the chosen layer are skipped. Far enough to
// move the ribbon by less than one pixel at 2560x1440, the wander can be skipped; the end of the ORM fade can
// use just the layer chosen by wear. See the cart track section of reports/engine/2026-11-17-grass-cost.
const float GrassTrackHeight   = 140.0;  // of 256 of the wear: what the heights' difference is worth
const float GrassTrackFarFrom  = 1200.0; // cm: the hard choice of a layer passes to their mix
const float GrassTrackFarTo    = 1500.0;
const float GrassTrackFarSoft  = 34.0;   // cm: the foot the ribbon's maps are read by from afar
const float GrassTrackRibbonTo = 6000.0; // cm: no normal of the ribbon beyond; fading from 0.7 of it
const float GrassTrackRough    = 0.9;    // the roughness from afar
const float GrassTrackFarShade = 0.86;   // the layers' colour from afar: the shade of their grain
const float GrassTrackCrustTint = 0.6;   // of the ribbon's tint on the crust; the loose earth takes it whole
const float GrassTrackBody     = 0.86;   // the tint's lightest channel inside the ribbon: the builder's BODY
const vec3  GrassTrackStain    = vec3(0.78, 0.58, 0.58); // a stain of the body: the builder's STAIN
const float GrassTrackStainFrom = -0.10; // of the stains' noise: none below, whole from GrassTrackStainTo
const float GrassTrackStainTo  = 0.32;
const float GrassTrackStainRut = 4.0;    // by the tint's green under the body's: no stain in a rut from 1/4

// Bench bits (Grass::Part in grassdiag.h); substitute neutral material inputs for the disabled work.
const uint GrassPartTrackWander = 1u << 16;
const uint GrassPartTrackFade   = 1u << 17;
const uint GrassPartTrackRagged = 1u << 18;
const uint GrassPartTrackOrm    = 1u << 19;
const uint GrassPartTrackColor  = 1u << 20;
const uint GrassPartTrackSpots  = 1u << 21;
const uint GrassPartTrackTone   = 1u << 22;
const uint GrassPartTrackForm   = 1u << 23;
const uint GrassPartTrackGrain  = 1u << 24;
const uint GrassPartTrackNeutral   = 1u << 25;
const uint GrassPartTrackWanderLod = 1u << 26;
const uint GrassPartTrackOrmLod    = 1u << 27;

// the crust's lattice, turned the other way from the loose earth's
vec2 grassCrustUv(vec2 xz) { return vec2(0.84805*xz.x - 0.52992*xz.y, 0.52992*xz.x + 0.84805*xz.y); }

bool grassTrackSubpixelWander(vec3 pos, float wander, vec3 t, vec3 b, float det, vec2 edge,
                             vec2 duvdx, vec2 duvdy, vec3 dpdx, vec3 dpdy) {
  const float uvDet = duvdx.x*duvdy.y - duvdx.y*duvdy.x;
  if(abs(uvDet)<=1e-12)
    return false;
  // The slow noises have independent signs; the same quick noise is added to x and subtracted from z.
  const vec2 slow = abs(vec2(t.x,b.x)) + abs(vec2(t.z,b.z));
  const vec2 quick = 0.3*abs(vec2(t.x-t.z,b.x-b.z));
  const vec2 uvMax = min(vec2(0.1), (slow + quick)*(wander/abs(det)))*edge;
  // The finite differences give the affine UV-to-world transform even on a perspective triangle.
  const vec3 axisU = (dpdx*duvdy.y - dpdy*duvdx.y)/uvDet;
  const vec3 axisV = (dpdy*duvdx.x - dpdx*duvdy.x)/uvDet;
  const vec4 u = scene.viewProject*vec4(axisU, 0.0);
  const vec4 v = scene.viewProject*vec4(axisV, 0.0);
  const vec4 p = scene.viewProject*vec4(pos, 1.0);
  const float minDepth = p.w - dot(abs(vec2(u.w,v.w)), uvMax);
  if(minDepth<=0.0)
    return false;
  // Bound the difference after perspective division in both screen axes, with a margin for rounding.
  const vec2 ndc = p.xy/p.w;
  const vec2 upper = abs(u.xy - ndc*u.w)*uvMax.x + abs(v.xy - ndc*v.w)*uvMax.y;
  const vec2 halfResolution = 0.5*max(vec2(2560.0,1440.0), vec2(scene.screenRes));
  return length(upper*halfResolution)<0.98*minDepth;
  }

// tex: the crust's maps in the bindless arrays, then the loose earth's, then the ribbon's; crustK, looseK: repeats
// per cm; knobs: Grass::pack(Track); uv with its derivatives: the mesh's own, as its painted texture is read;
// strip: the share of a verge's strip of earth on this pixel (grassLayers)
void grassTrack(uint tex, float crustK, float looseK, float knobs, vec2 uv, vec2 duvdx, vec2 duvdy,
                vec3 pos, vec3 normal, vec3 dpdx, vec3 dpdy, float strip,
                inout vec4 diffuse, inout uint normalOut, inout vec4 orm) {
  const vec3  gn   = normalize(normal);
  const float lies = smoothstep(GrassLiesFrom, GrassLiesTo, gn.y);
  if(lies<=0.0)
    return;
  const uint  bits   = uint(knobs + 0.5);
  const float wander = float(bits & 255u)*(30.0/255.0);
  const float fade   = float((bits >> 8) & 255u)*(1.0/255.0);
  const float ragged = float((bits >> 16) & 255u)*(2.0/255.0);
  const ivec2 place  = ivec2(floor(pos.xz*16.0));
  const float mip    = scene.mipGradScale;
  const uint  rib    = tex + 2u;
  const uint  partsOff = scene.grassTrackPartsOff;
  const bool  neutral = (partsOff & GrassPartTrackNeutral)==0u;

  // the ribbon's frame on the ground: t and b along the growth of u and v, over det the gradients themselves
  const vec3  py  = cross(dpdy, gn), px = cross(gn, dpdx);
  const vec3  t   = py*duvdx.x + px*duvdy.x, b = py*duvdx.y + px*duvdy.y;
  const float det = dot(dpdx, py);
  const float d   = distance(pos, scene.camPos);
  const float far = smoothstep(GrassTrackFarFrom, GrassTrackFarTo, d);
  // the drawing wanders by the world: a slow sway and a quicker one; still at the tile's edges, where it meets
  // the next piece and the ground beside
  vec2 uvR = uv;
  if((partsOff & GrassPartTrackWander)==0u && wander>0.0 && abs(det)>1e-12) {
    const vec2  f = fract(uv);
    const vec2  edge = smoothstep(vec2(0.0), vec2(0.08), min(f, 1.0-f));
    const bool subpixel = far>=1.0 && (partsOff & GrassPartTrackWanderLod)==0u &&
                          grassTrackSubpixelWander(pos, wander, t, b, det, edge, duvdx, duvdy, dpdx, dpdy);
    if(!subpixel) {
      const float quick = grassTrackNoise(place,2560,0x73u)*0.3;
      const vec3  w = vec3(grassTrackNoise(place,11200,0x71u) + quick, 0.0,
                           grassTrackNoise(place + ivec2(5113,2971),11200,0x72u) - quick)*wander;
      uvR += clamp(vec2(dot(t,w), dot(b,w))/det, vec2(-0.1), vec2(0.1))*edge;
      }
    }
  // from afar the ribbon's maps are read by a wider foot, the same along and across the road: the fray and the
  // grain that break a rut's edge near are gone there, and its narrow dark bed would be a ruled line
  vec2 dxR = duvdx*mip, dyR = duvdy*mip;
  if(far>0.0 && abs(det)>1e-12) {
    const float soft = far*GrassTrackFarSoft/abs(det);
    dxR.x += length(t)*(dxR.x<0.0 ? -soft : soft);
    dyR.y += length(b)*(dyR.y<0.0 ? -soft : soft);
    }
  const vec3  mask = fringeTexture(2u, rib, uvR, dxR, dyR).rgb;
  const float own  = max(mask.b, strip);
  if(own<=0.004)
    return;
  // With a unit height the near share is still zero here, and so is the far share.
  if(neutral && (own<=(128.0 - 0.5*GrassTrackHeight)/256.0 || (far>=1.0 && own<=0.3)))
    return;

  // where the ruts fade: 1 at their full depth and tone; by 6 m and by 2.2 m - one rut of the two may fade alone
  float k = 1.0;
  if((partsOff & GrassPartTrackFade)==0u && (!neutral || fade>0.0))
    k = 1.0 - fade*smoothstep(0.05, 0.55, grassTrackNoise(place,9600,0x75u)*0.6 + grassTrackNoise(place,3520,0x76u)*0.4);
  float lean = (mask.g*mix(1.0, k, 0.5) + strip)*256.0 - 128.0;
  // Neither the winning layer nor the clamped far weight can change beyond the combined noise's amplitude.
  if((partsOff & GrassPartTrackRagged)==0u && (!neutral || (ragged>0.0 && abs(lean)<0.5*GrassTrackHeight + 130.0*ragged + 1.0)))
    lean += (grassTrackNoise(place,3840,0x77u)*90.0 + grassTrackNoise(place,560,0x78u)*40.0)*ragged;

  const vec2  uvC = grassCrustUv(pos.xz)*crustK, dxC = grassCrustUv(dpdx.xz)*crustK, dyC = grassCrustUv(dpdy.xz)*crustK;
  const vec2  uvL = grassLooseUv(pos.xz)*looseK, dxL = grassLooseUv(dpdx.xz)*looseK, dyL = grassLooseUv(dpdy.xz)*looseK;
  // the share of the loose earth: near one layer or the other, from afar their mix
  float wl = clamp(lean/GrassTrackHeight + 0.5, 0.0, 1.0);
  vec3  o  = vec3(1.0, GrassTrackRough, 0.5);
  bool  loose = lean>0.0;
  if(far<1.0) {
    vec3 own0 = o;
    if((partsOff & GrassPartTrackOrm)==0u) {
      const bool oneTap = far>=254.0/255.0 && (partsOff & GrassPartTrackOrmLod)==0u;
      if(neutral || oneTap) {
        own0 = fringeTexture(2u, tex+uint(loose), loose ? uvL : uvC,
                             (loose ? dxL : dxC)*mip, (loose ? dyL : dyC)*mip).rgb;
        if(!oneTap && abs(lean)<0.5*GrassTrackHeight) {
          // The second height is in [0,1]. Read it only if it can overturn the expected winner.
          const bool uncertain = loose ? lean + (own0.b-1.0)*GrassTrackHeight<=0.0 :
                                         lean + (1.0-own0.b)*GrassTrackHeight>0.0;
          if(uncertain) {
            const vec3 other = fringeTexture(2u, tex+uint(!loose), loose ? uvC : uvL,
                                             (loose ? dxC : dxL)*mip, (loose ? dyC : dyL)*mip).rgb;
            const float delta = loose ? own0.b-other.b : other.b-own0.b;
            const bool pickLoose = lean + delta*GrassTrackHeight>0.0;
            own0 = loose==pickLoose ? own0 : other;
            loose = pickLoose;
            }
          }
        } else if(abs(lean)<0.5*GrassTrackHeight) {
        const vec3 oC = fringeTexture(2u, tex,    uvC, dxC*mip, dyC*mip).rgb;
        const vec3 oL = fringeTexture(2u, tex+1u, uvL, dxL*mip, dyL*mip).rgb;
        loose = lean + (oL.b-oC.b)*GrassTrackHeight>0.0;
        own0  = loose ? oL : oC;
        } else {
        own0  = loose ? fringeTexture(2u, tex+1u, uvL, dxL*mip, dyL*mip).rgb : fringeTexture(2u, tex, uvC, dxC*mip, dyC*mip).rgb;
        }
      }
    wl = mix(loose ? 1.0 : 0.0, wl, far);
    o  = mix(own0, o, far);
    }
  // the ribbon's edge outside a meadow and the end of a verge's strip: the layers' height moves where the paint begins
  const float share = lies*(own>=1.0 ? 1.0 : mix(own*256.0 + (o.b-0.5)*GrassTrackHeight>128.0 ? 1.0 : 0.0,
                                                 smoothstep(0.3, 0.7, own), far));
  if(share<=0.0)
    return;

  if((grassPartsOff(scene.grassRing.w) & GrassPartCover)!=0u) {
    // the cover's paint: the strip as the layers paint it, the road's own crust blue and loose earth cyan
    diffuse.rgb = strip>0.0 ? mix(vec3(1.0,0.0,1.0), vec3(0.8,0.2,0.0), strip) : mix(vec3(0.0,0.0,1.0), vec3(0.0,0.8,0.8), wl);
    return;
    }

  vec3 albedo = vec3(0);
  if((partsOff & GrassPartTrackColor)==0u) {
    if(wl<1.0)
      albedo = fringeTexture(0u, tex, uvC, dxC*mip, dyC*mip).rgb;
    if(wl>0.0)
      albedo = mix(albedo, fringeTexture(0u, tex+1u, uvL, dxL*mip, dyL*mip).rgb, wl);
    } else {
    albedo = vec3(1.0);
    }
  const vec3  tint   = fringeTexture(0u, rib, uvR, dxR, dyR).rgb;
  // the stains by the world: by 2.6 m and by 0.7 m
  const float inside = clamp((1.0 - max(tint.r, max(tint.g, tint.b)))*(1.0/(1.0 - GrassTrackBody)), 0.0, 1.0);
  const float rut    = clamp((GrassTrackBody - tint.g)*GrassTrackStainRut, 0.0, 1.0)*k;
  float stain = 0.0;
  if((partsOff & GrassPartTrackSpots)==0u && (!neutral || (inside>0.0 && rut<1.0 && strip<1.0))) {
    const float spots = grassTrackNoise(place,4160,0x7Du)*0.55 + grassTrackNoise(place,1120,0x7Eu)*0.45;
    stain = smoothstep(GrassTrackStainFrom, GrassTrackStainTo, spots)*inside*(1.0 - rut)*(1.0 - strip);
    }
  const float tone = (partsOff & GrassPartTrackTone)==0u ? grassTrackTone(place) : 1.0;
  albedo *= mix(vec3(1.0), tint, k*mix(GrassTrackCrustTint, 1.0, wl))*mix(vec3(1.0), GrassTrackStain, stain)*
            (tone*mix(1.0, GrassTrackFarShade, far));

  // the normal: the ribbon's large forms, the layer's grain over them
  vec3 n = gn;
  const float form = k*(1.0 - smoothstep(0.7*GrassTrackRibbonTo, GrassTrackRibbonTo, d));
  if((partsOff & GrassPartTrackForm)==0u && form>0.0) {
    const float lt = dot(t,t), lb = dot(b,b);
    if(lt>1e-20 && lb>1e-20) {
      // each axis by its own length: the ribbon's maps are made for its metres across and along
      const vec2 xy = (fringeTexture(1u, rib, uvR, dxR, dyR).ag*2.0 - 1.0)*form;
      n = normalize(t*(inversesqrt(lt)*xy.x) + b*(inversesqrt(lb)*xy.y) + gn*sqrt(max(0.0, 1.0 - dot(xy,xy))));
      }
    }
  if((partsOff & GrassPartTrackGrain)==0u && d<GrassLayerNormalTo && far<1.0) {
    const vec4 nm = loose ? fringeTexture(1u, tex+1u, uvL, dxL*mip, dyL*mip) : fringeTexture(1u, tex, uvC, dxC*mip, dyC*mip);
    const vec3 fine = mappedNormal(n, dpdx, dpdy, loose ? dxL : dxC, loose ? dyL : dyC, nm);
    n = normalize(mix(fine, n, far));
    }
  // the blue was the height: neither layer has metal
  const vec3 own1 = vec3(o.r*mix(1.0, mask.r, k), o.g, 0.0);
  if(share<1.0) {
    diffuse.rgb = mix(diffuse.rgb, albedo, share);
    normalOut   = encodeNormal(normalize(mix(decodeNormal(normalOut), n, share)));
    orm.rgb     = mix(orm.rgb, own1, share);
    } else {
    diffuse.rgb = albedo;
    normalOut   = encodeNormal(n);
    orm.rgb     = own1;
    }
  orm.a = 1.0;
  }

// The ground's own material, whichever its record has: the meadow's two layers (BK_GROUND) with the road's earth
// in its notches, the strip of them along a road (BK_VERGE), the road's own track (BK_TRACK). k0, k1, knobs:
// Bucket::fringeBottom0, fringeBottom1, foliageReach. Returns what grassLayers does
float grassOwnGround(uint flags, uint tex, float swardK, float earthK, float farK, float k0, float k1, float knobs,
                     vec2 uv, vec2 duvdx, vec2 duvdy, vec3 pos, vec3 normal, vec3 dpdx, vec3 dpdy,
                     inout vec4 diffuse, inout uint normalOut, inout vec4 orm) {
  const float ringW = scene.grassRing.w;
  if(ringW<=0.0)
    return -1.0;
  const bool verge    = (flags & BK_VERGE)!=0u;
  // the bench's: the road as with its verge alone, the meadow's notches its own earth
  const bool trackOff = (grassPartsOff(ringW) & GrassPartTrack)!=0u;
  const bool track    = (flags & BK_TRACK)!=0u && !trackOff;
  float sward = -1.0, strip = 0.0;
  if((flags & (BK_GROUND | BK_VERGE))!=0u) {
    const float otherK = verge ? (track ? k1 : 0.0) : (trackOff ? 0.0 : k0);
    sward = grassLayers(flags, tex, swardK, earthK, farK, tex + (verge ? 3u : 2u), otherK, pos, normal, dpdx, dpdy,
                        diffuse, normalOut, orm, strip);
    }
  if(track && sward==-1.0)
    grassTrack(tex + (verge ? 2u : 0u), k0, k1, knobs, uv, duvdx, duvdy, pos, normal, dpdx, dpdy, strip,
               diffuse, normalOut, orm);
  return sward;
  }

#endif

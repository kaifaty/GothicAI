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

// the crust's lattice, turned the other way from the loose earth's
vec2 grassCrustUv(vec2 xz) { return vec2(0.84805*xz.x - 0.52992*xz.y, 0.52992*xz.x + 0.84805*xz.y); }

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

  // the ribbon's frame on the ground: t and b along the growth of u and v, over det the gradients themselves
  const vec3  py  = cross(dpdy, gn), px = cross(gn, dpdx);
  const vec3  t   = py*duvdx.x + px*duvdy.x, b = py*duvdx.y + px*duvdy.y;
  const float det = dot(dpdx, py);
  // the drawing wanders by the world: a slow sway and a quicker one; still at the tile's edges, where it meets
  // the next piece and the ground beside
  vec2 uvR = uv;
  if(wander>0.0 && abs(det)>1e-12) {
    const float quick = grassTrackNoise(place,2560,0x73u)*0.3;
    const vec3  w = vec3(grassTrackNoise(place,11200,0x71u) + quick, 0.0,
                         grassTrackNoise(place + ivec2(5113,2971),11200,0x72u) - quick)*wander;
    const vec2  f = fract(uv);
    uvR += clamp(vec2(dot(t,w), dot(b,w))/det, vec2(-0.1), vec2(0.1))*smoothstep(vec2(0.0), vec2(0.08), min(f, 1.0-f));
    }
  // from afar the ribbon's maps are read by a wider foot, the same along and across the road: the fray and the
  // grain that break a rut's edge near are gone there, and its narrow dark bed would be a ruled line
  const float d   = distance(pos, scene.camPos);
  const float far = smoothstep(GrassTrackFarFrom, GrassTrackFarTo, d);
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

  // where the ruts fade: 1 at their full depth and tone; by 6 m and by 2.2 m - one rut of the two may fade alone
  const float k    = 1.0 - fade*smoothstep(0.05, 0.55, grassTrackNoise(place,9600,0x75u)*0.6 +
                                                       grassTrackNoise(place,3520,0x76u)*0.4);
  const float lean = (mask.g*mix(1.0, k, 0.5) + strip)*256.0 - 128.0 +
                     (grassTrackNoise(place,3840,0x77u)*90.0 + grassTrackNoise(place,560,0x78u)*40.0)*ragged;

  const vec2  uvC = grassCrustUv(pos.xz)*crustK, dxC = grassCrustUv(dpdx.xz)*crustK, dyC = grassCrustUv(dpdy.xz)*crustK;
  const vec2  uvL = grassLooseUv(pos.xz)*looseK, dxL = grassLooseUv(dpdx.xz)*looseK, dyL = grassLooseUv(dpdy.xz)*looseK;
  // the share of the loose earth: near one layer or the other, from afar their mix
  float wl = clamp(lean/GrassTrackHeight + 0.5, 0.0, 1.0);
  vec3  o  = vec3(1.0, GrassTrackRough, 0.5);
  bool  loose = lean>0.0;
  if(far<1.0) {
    vec3 own0;
    if(abs(lean)<0.5*GrassTrackHeight) {
      const vec3 oC = fringeTexture(2u, tex,    uvC, dxC*mip, dyC*mip).rgb;
      const vec3 oL = fringeTexture(2u, tex+1u, uvL, dxL*mip, dyL*mip).rgb;
      loose = lean + (oL.b-oC.b)*GrassTrackHeight>0.0;
      own0  = loose ? oL : oC;
      } else {
      own0  = loose ? fringeTexture(2u, tex+1u, uvL, dxL*mip, dyL*mip).rgb : fringeTexture(2u, tex, uvC, dxC*mip, dyC*mip).rgb;
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
  if(wl<1.0)
    albedo = fringeTexture(0u, tex, uvC, dxC*mip, dyC*mip).rgb;
  if(wl>0.0)
    albedo = mix(albedo, fringeTexture(0u, tex+1u, uvL, dxL*mip, dyL*mip).rgb, wl);
  const vec3  tint   = fringeTexture(0u, rib, uvR, dxR, dyR).rgb;
  // the stains by the world: by 2.6 m and by 0.7 m
  const float inside = clamp((1.0 - max(tint.r, max(tint.g, tint.b)))*(1.0/(1.0 - GrassTrackBody)), 0.0, 1.0);
  const float spots  = grassTrackNoise(place,4160,0x7Du)*0.55 + grassTrackNoise(place,1120,0x7Eu)*0.45;
  const float rut    = clamp((GrassTrackBody - tint.g)*GrassTrackStainRut, 0.0, 1.0)*k;
  const float stain  = smoothstep(GrassTrackStainFrom, GrassTrackStainTo, spots)*inside*(1.0 - rut)*(1.0 - strip);
  albedo *= mix(vec3(1.0), tint, k*mix(GrassTrackCrustTint, 1.0, wl))*mix(vec3(1.0), GrassTrackStain, stain)*
            (grassTrackTone(place)*mix(1.0, GrassTrackFarShade, far));

  // the normal: the ribbon's large forms, the layer's grain over them
  vec3 n = gn;
  const float form = k*(1.0 - smoothstep(0.7*GrassTrackRibbonTo, GrassTrackRibbonTo, d));
  if(form>0.0) {
    const float lt = dot(t,t), lb = dot(b,b);
    if(lt>1e-20 && lb>1e-20) {
      // each axis by its own length: the ribbon's maps are made for its metres across and along
      const vec2 xy = (fringeTexture(1u, rib, uvR, dxR, dyR).ag*2.0 - 1.0)*form;
      n = normalize(t*(inversesqrt(lt)*xy.x) + b*(inversesqrt(lb)*xy.y) + gn*sqrt(max(0.0, 1.0 - dot(xy,xy))));
      }
    }
  if(d<GrassLayerNormalTo && far<1.0) {
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

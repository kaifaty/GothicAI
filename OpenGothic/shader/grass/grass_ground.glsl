#ifndef GRASS_GROUND_GLSL
#define GRASS_GROUND_GLSL

#include "grass/grass_common.glsl"
#include "grass/grass_cover.glsl"
#include "materials/foliage_wind.glsl"

// The ground of a material with `grass` (BK_GRASS; reports/engine/2026-11-14-grass-system), in the landscape's
// G-buffer pass with material maps. Seen from above, near the camera, the ground between the blades lies in their
// shade. Seen along the meadow, or from further than the blades stand at their full count, it takes the colour of
// the grass from afar, since there it stands for the blades behind the drawn ones and for the thinned ones: the
// colour alone, the light stays the ground's. From half of the distance where the
// blades start to go, to that distance, it takes the whole of the far look and keeps it beyond: the blades then
// vanish against an equal ground, and the meadow does not end at the ring.
// The mark of an own material (gbufOrm.a) is written at any distance: the sun through the far grass is read only
// under it, and a mark that came in with the distance would be a step of the light. A ground with maps of its own
// keeps its mark and what the maps wrote: the far look is mixed in over them. Its metal is not kept where the far
// look begins: under the foliage bit that channel is the share of the sun let through.
// sward: of the ground's own layers (grass_layers.glsl), 1 a pixel of sward, 0 of earth; under 0 without them: the
// spots of bare earth are then the first round's, a tint of the painted ground
void grassGround(uint bits, vec3 pos, vec3 normal, float sward, inout vec4 diffuse, inout uint normalOut, inout vec4 orm) {
  const vec4 ring = scene.grassRing;
  if(ring.w<=0.0)
    return;
  if(orm.a<=0.0)
    orm.a = 1.0;
  if(ring.w<0.75)
    return; // the mark alone: the pair that shows what it does to the near ground

  const uint  partsOff = grassPartsOff(ring.w);
  if((partsOff & GrassPartPaint)!=0u) {
    // with the ground's shade off as well: the whole ground of the grass, the mask of the tone's measure
    diffuse.rgb = (partsOff & GrassPartGroundShade)==0u ? grassPaintBand(distance(pos.xz, scene.camPos.xz)) :
                  ((partsOff & GrassPartCells)!=0u ? grassPaintCells(pos.xz) : vec3(1,0,1));
    return;
    }
  const vec3  gn       = normalize(normal);
  const float slopeMax = grassPackedSlope(bits);
  const float tanS     = length(gn.xz)/max(gn.y, 1e-3);
  const float slope    = slopeMax>0.0 ? 1.0 - smoothstep(tan(radians(max(slopeMax-10.0,0.0))), tan(radians(slopeMax)), tanS) : 0.0;
  const float grows    = min(grassPackedDensity(bits),1.0)*slope;
  if(grows<=0.0)
    return;
  // a spot of bare earth in the sward: earth, lit as the ground it is
  const ivec2 place    = ivec2(floor(pos.xz*16.0));
  float bare = 1.0 - sward;
  if(sward<0.0) {
    bare         = grassBare(place);
    diffuse.rgb *= mix(vec3(1.0), GrassEarthTint, bare*grows);
    }
  else if(sward<0.5) {
    // the earth of a spot, not of a border: there it stays the road's
    diffuse.rgb *= mix(vec3(1.0), GrassSpotEarth, smoothstep(0.2, 0.5, float(grassCoverBare(place))/256.0));
    }
  const int   noise    = grassPatch(place);
  if(sward>0.5)
    diffuse.rgb *= mix(GrassSwardDry, GrassSwardLush, smoothstep(GrassSwardDryAt, GrassSwardLushAt, float(noise)));
  const float stand    = grassStand(noise)*(1.0-bare)*grows;
  if(stand<=0.0)
    return;

  const vec3  toEye = scene.camPos - pos;
  const float d     = length(toEye);
  const bool  plain = (partsOff & GrassPartFar)!=0u;
  const float far   = plain ? 0.0 : smoothstep(ring.y*0.5, ring.y, d);
  const float graze = plain ? 0.0 : 1.0 - smoothstep(GrassGrazeFrom, GrassGrazeTo, dot(gn,toEye)/max(d,1.0));
  const float nearShade = (partsOff & GrassPartGroundShade)!=0u ? 1.0 : (sward>=0.0 ? GrassOwnGroundShade : GrassGroundShade);
  const float along = plain ? 0.0 : max(max(far,graze), smoothstep(ring.x, ring.y*0.5, d));
  vec3 tint = mix(vec3(mix(1.0, nearShade, stand)), mix(vec3(1.0), GrassFarTint, stand), along);
  if(far>0.0) {
    // the far grass under the gust: the pressure of the place tilts the normal and lightens the tone, as the
    // blades' own (grass.vert), so the band of the gust does not stop where the blades end
    const vec3  loc   = foliageFrontAt(pos.z, scene.foliageFront.xy, scene.foliageFrontK, scene.foliageWind, scene.foliageSwing);
    const float lean  = tan(min(grassLeanAngle(grassPackedHeight(bits))*loc.x*grassPackedWind(bits), 0.698))*scene.foliageWind.w;
    const float k     = stand*far;
    tint     *= 1.0 + GrassLeanSheen*k*min(lean/(0.5*GrassLeanMaxTan), 1.0);
    normalOut = encodeNormal(normalize(mix(decodeNormal(normalOut), gn, k) - vec3(scene.foliageWind.x, 0.0, scene.foliageWind.y)*(lean*GrassNormalLean*k)));
    orm.r     = mix(orm.r, GrassFarAo, k);
    orm.g     = mix(orm.g, GrassRoughness, far);
    orm.b     = grassPackedThrough(bits)*k;
    diffuse.a = float(int(diffuse.a*255.0 + 0.5) | GBufFoliageBit)/255.0;
    }
  diffuse.rgb = min(diffuse.rgb*tint, vec3(1.0));
  }

#endif

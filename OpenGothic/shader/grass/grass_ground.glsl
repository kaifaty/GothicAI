#ifndef GRASS_GROUND_GLSL
#define GRASS_GROUND_GLSL

#include "grass/grass_common.glsl"
#include "materials/foliage_wind.glsl"

// The ground of a material with `grass` (BK_GRASS; reports/engine/2026-11-14-grass-system), in the landscape's
// G-buffer pass with material maps. Near the camera the ground between the blades lies in their shade. From half
// of the distance where the blades start to go, to that distance, it takes what the grass looks like from afar
// and keeps it beyond: the blades then vanish against an equal ground, and the meadow does not end at the ring.
// The mark of an own material (gbufOrm.a) is written at any distance: the sun through the far grass is read only
// under it, and a mark that came in with the distance would be a step of the light. A ground with maps of its own
// keeps its mark and what the maps wrote: the far look is mixed in over them. Its metal is not kept where the far
// look begins: under the foliage bit that channel is the share of the sun let through.
void grassGround(uint bits, vec3 pos, vec3 normal, inout vec4 diffuse, inout uint normalOut, inout vec4 orm) {
  const vec4 ring = scene.grassRing;
  if(ring.w<=0.0)
    return;
  if(orm.a<=0.0)
    orm.a = 1.0;
  if(ring.w<0.75)
    return; // the mark alone: the pair that shows what it does to the near ground

  const vec3  gn       = normalize(normal);
  const float slopeMax = grassPackedSlope(bits);
  const float tanS     = length(gn.xz)/max(gn.y, 1e-3);
  const float slope    = slopeMax>0.0 ? 1.0 - smoothstep(tan(radians(max(slopeMax-10.0,0.0))), tan(radians(slopeMax)), tanS) : 0.0;
  const float stand    = grassStand(grassPatch(ivec2(floor(pos.xz*16.0))))*min(grassPackedDensity(bits),1.0)*slope;
  if(stand<=0.0)
    return;

  const float d   = distance(pos, scene.camPos);
  const float far = smoothstep(ring.y*0.5, ring.y, d);
  vec3 tint = mix(vec3(mix(1.0, GrassGroundShade, stand)), mix(vec3(1.0), GrassFarTint, stand), far);
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

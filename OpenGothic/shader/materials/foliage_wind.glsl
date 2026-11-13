#ifndef FOLIAGE_WIND_GLSL
#define FOLIAGE_WIND_GLSL

// Living foliage (reports/engine/2026-11-14-living-foliage): the vertices of a material with `foliage` in
// materials.json (BK_FOLIAGE) move in the world's wind. The numbers and the same rule on the processor, for the
// tests and the bounds: common/graphics/foliagewind.h. Keep the two files in step.
//
// A tuft's own axes: X along the wall, Y up the surface, Z out of the rock; the roots lie on X. A vertex swings
// about the point of X over it and keeps the length of its arm: a hanging leaf is a pendulum, a standing one bends.
// The wind is the scene's (SceneDesc::foliageWind: xy its direction in the world's x and z, z its pressure, w 1
// when it blows; foliagePhase: 0..1 of the periods of the two waves and the two flutters).

const float FoliageMaxScale    = 4.0;
const float FoliageBendK       = 0.162;
const float FoliageBendMaxTan  = 0.3093;
const float FoliageBendRoot    = 0.35;
const float FoliageWaveSteady  = 0.5;
const float FoliageWaveSwing   = 0.5;
const float FoliageWave1Length = 2400.0;
const float FoliageWave2Length = 950.0;
const float FoliageWave1Share  = 0.55;
const float FoliageWave2Share  = 0.45;
const vec2  FoliageWave2Dir    = vec2(0.819152, 0.573576);
const float FoliageFlutterCalm = 0.004;
const float FoliageFlutterWind = 0.022;
const float FoliageFlutterMaxQ = 1.5;
const float FoliageFlutterLeaf = 0.35;
const float FoliageWallSign    = 0.04;
const float FoliageLeeLift     = 0.35;

float foliagePackedWind   (uint bits) { return float( bits       & 0xFFu)*(FoliageMaxScale/255.0); }
float foliagePackedFlutter(uint bits) { return float((bits >> 8) & 0xFFu)*(FoliageMaxScale/255.0); }
// the share of the sun a leaf lets through (main.frag writes it into the G-buffer)
float foliageTranslucency (uint bits) { return float((bits >> 16) & 0xFFu)/255.0; }
bool  foliageMoves        (uint bits) { return (bits & 0xFFFFu)!=0u; }

float foliagePlaceHash(vec3 place) {
  uint h = uint(int(floor(place.x)))*0x9E3779B1u ^ uint(int(floor(place.z)))*0x85EBCA77u;
  h ^= h >> 15;
  h *= 0x2C1B3C6Du;
  h ^= h >> 12;
  return float(h & 0xFFFFu)/65536.0;
  }

// p: the vertex in the tuft's axes; reach: the largest arm of the mesh at rest (Bucket::foliageReach); mat: the
// object's matrix. The result is the vertex moved, in the same axes
vec3 foliageBend(vec3 p, uint bits, float reach, mat4x3 mat, vec4 wind, vec4 phase) {
  const float windK = foliagePackedWind(bits), flutK = foliagePackedFlutter(bits);
  if(wind.w<=0.0 || (windK<=0.0 && flutK<=0.0))
    return p;
  const float len = length(p.yz);
  if(len<1e-4)
    return p;
  const float s = min(len/max(reach,1e-3), 1.0);
  const float q = wind.z;

  // the wind in the tuft's axes: what the wall stops turns along it and keeps its speed; none goes into the rock
  const vec3  w  = vec3(wind.x, 0.0, wind.y);
  const vec3  al = vec3(length(mat[0]), length(mat[1]), length(mat[2]));
  const vec3  wl = vec3(al.x>1e-6 ? dot(w,mat[0])/al.x : 0.0,
                        al.y>1e-6 ? dot(w,mat[1])/al.y : 0.0,
                        al.z>1e-6 ? dot(w,mat[2])/al.z : 0.0);
  const float own   = wl.x;
  const float into  = max(-wl.z,0.0);
  const float along = own + (length(vec2(own,into)) - abs(own))*own/sqrt(own*own + FoliageWallSign*FoliageWallSign);
  const vec3  dir   = vec3(along, wl.y, FoliageLeeLift*max(wl.z,0.0));

  // the gusts: two waves that run over the world
  const vec3  place = mat[3];
  const float w1 = sin(6.2831853*(fract(place.z/FoliageWave1Length) - phase.x));
  const float w2 = sin(6.2831853*(fract(dot(place.xz,FoliageWave2Dir)/FoliageWave2Length) - phase.y));
  const float g    = FoliageWave1Share*w1 + FoliageWave2Share*w2;
  const float tanT = min(FoliageBendK*windK*q*(FoliageWaveSteady + FoliageWaveSwing*g), FoliageBendMaxTan);
  vec3 d = dir*(len*tanT*mix(FoliageBendRoot, 1.0, s));

  // the flutter: each tuft and each leaf of it in its own step, the tip most
  const float h    = foliagePlaceHash(place);
  const float leaf = p.x*FoliageFlutterLeaf;
  const float amp  = len*s*flutK*(FoliageFlutterCalm + FoliageFlutterWind*min(q,FoliageFlutterMaxQ));
  d.x += amp*0.8*sin(6.2831853*(phase.z + h) + leaf);
  d.z += amp*0.6*sin(6.2831853*(phase.w + fract(h*1.618034)) + leaf*1.3 + 1.7);

  const vec3  r  = vec3(d.x, p.y + d.y, p.z + d.z);
  const float rl = length(r);
  if(rl<1e-6)
    return p;
  return vec3(p.x,0.0,0.0) + r*(len/rl);
  }

#endif

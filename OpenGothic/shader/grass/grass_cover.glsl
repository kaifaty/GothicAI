#ifndef GRASS_COVER_GLSL
#define GRASS_COVER_GLSL

#include "grass/grass_common.glsl"

// Where the sward lies on a meadow's own ground (reports/engine/2026-11-14-meadow-ground): the rule of
// common/graphics/grass/grasscover.h, in the same whole numbers at a place in 1/16 cm, so the ground's two layers
// end where the blades do. Keep the numbers here and there in step.
const int GrassCoverNotch  = 20;
const int GrassCoverSpan   = 110;
const int GrassCoverRipple = 15;
const int GrassCoverSoft   = 40;
const int GrassCoverInside = 80;
const int GrassCoverReach  = 90;
const int GrassCoverSward  = 128;
const int GrassCoverTuft   = 72;

int grassCoverBeyond(int reach) { return reach + GrassCoverRipple + GrassCoverSoft; }

// Grass::coverBare: a spot of bare earth, 0..256
int grassCoverBare(ivec2 p) {
  const int step = 6400, radius = 480, spread = 640, soft = 480;
  const int cx = grassFloorDiv(p.x-step/2,step), cz = grassFloorDiv(p.y-step/2,step);
  int  bare = 0, bend = 0;
  bool bent = false;
  for(int j=cz; j<=cz+1; ++j)
    for(int i=cx; i<=cx+1; ++i) {
      const uint h = grassHash(i,j,0xB3u);
      if((h & 0xFFu)>=90u)
        continue;
      const ivec2 c = ivec2(i,j)*step + ivec2((((h >> 8) & 0xFFFu)*uint(step)) >> 12, (((h >> 20) & 0xFFFu)*uint(step)) >> 12);
      const int   r = radius + int(((grassHash(i,j,0xB4u) & 0xFFu)*uint(spread)) >> 8);
      const ivec2 d = p-c;
      const int   d2 = d.x*d.x + d.y*d.y; // under 1.5 of the step either way: no overflow
      if(d2>=4*r*r || grassPatch(c)>=int(GrassPatchMain0))
        continue;
      if(!bent) {
        bend = (grassValueNoise(p,1280,0xB6u)-32640)*435/32640 + (grassValueNoise(p,384,0xB7u)-32640)*166/32640;
        bent = true;
        }
      const int warp = clamp(bend*r/256, -r*9/10, r*9/10);
      bare = max(bare, clamp((r-int(sqrt(float(d2)))-warp)*256/soft + 128, 0, 256));
      }
  return bare;
  }

// Grass::coverBorder: the sward at a border, 0..256. s: 1/16 cm to the border, under 0 in the meadow; reach: cm
int grassCoverBorder(ivec2 p, int s, int reach) {
  if(s<=-GrassCoverInside*16)
    return 256;
  if(s>=grassCoverBeyond(reach)*16)
    return 0;
  const int slow = (3*grassValueNoise(p,3840,0x61u) + grassValueNoise(p+ivec2(911,1777),960,0x62u)) >> 2;
  const int t    = clamp((slow-15000)*256/(56000-15000), 0, 256);
  int line = -GrassCoverNotch*16 + ((GrassCoverSpan*16*t) >> 8);
  if(line>0)
    line = line*reach/GrassCoverReach;
  line += (grassValueNoise(p,384,0x63u)-32640)*(GrassCoverRipple*16)/32640;
  return clamp((line-s)*256/(GrassCoverSoft*16) + 128, 0, 256);
  }

// Grass::coverLevel: with the tufts' noise on what is neither none nor whole
int grassCoverLevel(ivec2 p, int border) {
  const int level = min(border, 256-grassCoverBare(p));
  if(level<=0 || level>=256)
    return level;
  return level + (grassValueNoise(p,288,0x64u)-32640)*GrassCoverTuft/32640;
  }

#endif
